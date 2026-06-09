from datetime import datetime, timezone, timedelta
import logging
from pathlib import Path
from fastapi import FastAPI, HTTPException, Request, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from sqlalchemy import text

from worker.celery_app import celery_app
from worker.db import engine
from worker.tasks import send_whatsapp_power_message, generate_power_report, FeederObj

# Logger configuration
# logging.basicConfig(level=logging.INFO)
# logger = logging.getLogger("WorkerGateway")


# Configure root logging to output to console only
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s %(message)s",
    handlers=[logging.StreamHandler()],
)

# Configure PowerMonitor logger to output to both console (via propagation) and file
logger = logging.getLogger("PowerMonitor")
logger.setLevel(logging.INFO)

log_dir = Path(__file__).parent / "logs"
log_dir.mkdir(exist_ok=True)
log_file = log_dir / "app.log"
file_handler = logging.FileHandler(log_file)
file_handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s %(message)s"))
logger.addHandler(file_handler)

app = FastAPI(title="FastAPI Worker Gateway API")



from fastapi.responses import PlainTextResponse, Response

from fastapi.exceptions import RequestValidationError
from fastapi import Request
from typing import Optional
from pydantic import BaseModel, Field


def _clean_validation_error(err):
    if isinstance(err, dict):
        return {k: _clean_validation_error(v) for k, v in err.items()}
    elif isinstance(err, (list, tuple)):
        return [_clean_validation_error(item) for item in err]
    elif isinstance(err, bytes):
        return err.decode("utf-8", errors="replace")
    elif isinstance(err, (str, int, float, bool, type(None))):
        return err
    return repr(err)

@app.exception_handler(RequestValidationError)
async def validation_exception_handler(request: Request, exc: RequestValidationError):
    logger.error(f"Validation error details: {exc.errors()}")
    logger.error(f"Raw body sent: {await request.body()}")
    cleaned_errors = _clean_validation_error(exc.errors())
    return JSONResponse(status_code=422, content={"detail": cleaned_errors})

    class PowerStatus(BaseModel):
        status: str
        timestamp: int
        peak_a0: int
        feeder_name: str
        # Use Field aliases to match incoming variations or guarantee a fallback default
        transformer_name: str = Field(default="UNKNOWN_TRANSFORMER", alias="transformer")
        sim_serial: Optional[str] = "UNKNOWN"
        contact_phone: Optional[str] = None
        msisdn: str = "UNKNOWN"

        class Config:
            populate_by_name = True  # Allows parsing both alias and field name keys
        
        
def save_power_status_update(data: PowerStatus, server_time_dt):
    if not data.sim_serial:
        if data.contact_phone:
            data.sim_serial = data.contact_phone
        elif data.msisdn and data.msisdn != "UNKNOWN":
            data.sim_serial = data.msisdn
        else:
            data.sim_serial = "UNKNOWN"
            
    lagos_tz = timezone(timedelta(hours=1))
    now_local = datetime.now(lagos_tz)

    try:
        with engine.begin() as conn:
            # Check if feeder exists
            # Note: Feeder table now uses `registered_phone` instead of `contact_phone`
            feeder_query = text("SELECT id, transformer_name, sim_serial, msisdn FROM myapp_feeder WHERE name = :name")
            feeder = conn.execute(feeder_query, {"name": data.feeder_name}).fetchone()
            
            if not feeder:
                # Create feeder
                insert_feeder_query = text("""
                    INSERT INTO myapp_feeder (name, transformer_name, sim_serial, msisdn, band, created_at)
                    VALUES (:name, :transformer_name, :sim_serial, :msisdn, 'A', :created_at)
                    RETURNING id
                """)
                feeder_id = conn.execute(insert_feeder_query, {
                    "name": data.feeder_name,
                    "transformer_name": data.transformer_name,
                    "sim_serial": data.sim_serial,
                    "msisdn": data.msisdn,
                    "created_at": now_local
                }).scalar()
            else:
                feeder_id = feeder[0]
                # Update feeder fields if they changed
                if feeder[1] != data.transformer_name or feeder[2] != data.sim_serial or feeder[3] != data.msisdn:
                    update_feeder_query = text("""
                        UPDATE myapp_feeder
                        SET transformer_name = :transformer_name, sim_serial = :sim_serial, msisdn = :msisdn
                        WHERE id = :id
                    """)
                    conn.execute(update_feeder_query, {
                        "transformer_name": data.transformer_name,
                        "sim_serial": data.sim_serial,
                        "msisdn": data.msisdn,
                        "id": feeder_id
                    })
            
            # Save power status
            # PowerStatus now stores `sim_serial` instead of `contact_phone`
            insert_status_query = text("""
                INSERT INTO myapp_powerstatus (feeder_id, status, timestamp, peak_a0, server_time, sim_serial, msisdn)
                VALUES (:feeder_id, :status, :timestamp, :peak_a0, :server_time, :sim_serial, :msisdn)
            """)
            conn.execute(insert_status_query, {
                "feeder_id": feeder_id,
                "status": data.status.upper(),
                "timestamp": data.timestamp,
                "peak_a0": data.peak_a0,
                "server_time": server_time_dt,
                "sim_serial": data.sim_serial,  # using sim_serial as the SIM identifier
                "msisdn": data.msisdn,
            })
            logger.info(f"Persisted power status update in database for feeder {data.feeder_name}")
            return feeder_id
    except Exception as e:
        logger.error(f"Error persisting power status update for feeder {data.feeder_name}: {e}", exc_info=True)
        raise e


@app.post("/power-tracker-gateway/")
async def power_update1(data: PowerStatus, request: Request):
    try:
        # Gracefully handle validation defaults if keys are absent or marked as UNKNOWN
        if not data.sim_serial or data.sim_serial == "UNKNOWN":
            if data.contact_phone:
                data.sim_serial = data.contact_phone
            elif data.msisdn and data.msisdn != "UNKNOWN":
                data.sim_serial = data.msisdn
            else:
                data.sim_serial = "UNKNOWN"
        lagos_tz = timezone(timedelta(hours=1))
        server_time_dt = datetime.now(lagos_tz)
        server_time = server_time_dt.strftime("%Y-%m-%d %H:%M:%S") + f".{int(server_time_dt.microsecond / 1000):03d}"
        
        logger.info(
            f"Incoming alert from Feeder: {data.feeder_name} [{data.transformer_name}] "
            f"-> Status: {data.status.upper()} (SIM ID: {data.msisdn})"
        )

        # --- Security Cross-Check ---
        try:
            # Match IMSI/MSISDN logic safely if identity strings are available
            if data.msisdn != "UNKNOWN" and data.sim_serial != "UNKNOWN":
                # NOTE: If you are checking if MSISDN equals IMSI, they will mismatch. 
                # Consider validating against a database record inside save_power_status_update instead.
                if data.msisdn.strip() == data.sim_serial.strip():
                    logger.info("Hardware telemetry transmission identity signature verified.")
        except Exception as celery_sec_err:
            logger.error(f"Failed to process security monitoring context logic: {celery_sec_err}")

        # --- Celery Worker Offload ---
        try:
            celery_app.send_task(
                "myapp.tasks.send_power_email", 
                args=[
                    data.feeder_name, 
                    data.status, 
                    data.timestamp, 
                    server_time, 
                    data.sim_serial,
                    data.transformer_name,
                    data.peak_a0,
                    data.msisdn,
                    data.sim_serial
                ]
            )
            logger.info("Grid status metric tracking update successfully offloaded to queue.")
        except Exception as celery_err:
            logger.error(f"Could not send main task to Celery: {celery_err}")   
        
    #     return {
    #         "status": "success",
    #         "queued_at": server_time,
    #         "node_validated": True
    #     }

    # except Exception as e:
    #     logger.error(f"Critical breakdown within gateway route context: {e}")
    #     return JSONResponse(
    #         status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
    #         content={"status": "error", "message": "Internal processing pipeline error"}
    #     )
        # --- Hardened SIM900 Response Termination ---
        # Explicitly passing 'Connection: close' tells the modem the session is finished
        return JSONResponse(
            status_code=status.HTTP_200_OK,
            headers={"Connection": "close"},
            content={
                "status": "success",
                "queued_at": server_time,
                "node_validated": True
            }
        )

    except Exception as e:
        logger.error(f"Critical breakdown within gateway route context: {e}")
        return JSONResponse(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            headers={"Connection": "close"},
            content={"status": "error", "message": "Internal processing pipeline error"}
        )
        
        
@app.get("/api/test-email/")
async def test_email(
    feeder_name: str = "Ayangbunren",
    contact_phone: str = "2348021299221"
):
    logger.info("Test email endpoint called")
    # Fetch Feeder from DB
    feeder = None
    try:
        with engine.connect() as conn:
            row = conn.execute(
                text("SELECT id, name, registered_phone, band FROM myapp_feeder WHERE name = :name"),
                {"name": feeder_name}
            ).fetchone()
            if row:
                feeder = FeederObj(row[0], row[1], row[2], row[3])
    except Exception as db_err:
        logger.error(f"Failed to fetch feeder for test_email: {db_err}")
        
    if not feeder:
        feeder = FeederObj(0, feeder_name, contact_phone, "A")
        
    lagos_tz = timezone(timedelta(hours=1))
    today_date = datetime.now(lagos_tz).date()
    server_time = datetime.now(lagos_tz).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    
    # Generate the power report body
    report_body = "Failed to generate report"
    try:
        report_body = generate_power_report(feeder, today_date, is_today=True)
    except Exception as rep_err:
        logger.error(f"Failed to generate report in test_email: {rep_err}")
        
    # Send email Celery task
    try:
        celery_app.send_task(
            "myapp.tasks.send_power_email", 
            args=[feeder.name, "ON", 9999, server_time, contact_phone]
        )
    except Exception as e:
        logger.error(f"Failed to enqueue test email task: {e}")
        
    # Send WhatsApp message
    whatsapp_status = "Failed"
    try:
        res = send_whatsapp_power_message(contact_phone, report_body)
        if res:
            whatsapp_status = "Sent"
    except Exception as wa_err:
        logger.error(f"Failed to send test WhatsApp message: {wa_err}")
        
    return {
        "status": "Success",
        "message": "Test email task sent to Celery queue",
        "whatsapp_status": whatsapp_status,
        "report_generated": report_body,
        "server_time": server_time
    }

@app.get("/api/test-power-email/")
async def test_power_email(
    feeder_name: str = "Erunwen Feeder",
    status: str = "ON",
    device_time: int = 1234567,
    contact_phone: str = "2348021299221"
):
    logger.info(f"Test power email endpoint called for feeder: {feeder_name}")
    
    # Fetch Feeder from DB
    feeder = None
    try:
        with engine.connect() as conn:
            row = conn.execute(
                text("SELECT id, name, registered_phone, band FROM myapp_feeder WHERE name = :name"),
                {"name": feeder_name}
            ).fetchone()
            if row:
                feeder = FeederObj(row[0], row[1], row[2], row[3])
    except Exception as db_err:
        logger.error(f"Failed to fetch feeder for test_power_email: {db_err}")
        
    if not feeder:
        feeder = FeederObj(0, feeder_name, contact_phone, "A")
        
    lagos_tz = timezone(timedelta(hours=1))
    today_date = datetime.now(lagos_tz).date()
    server_time = datetime.now(lagos_tz).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    
    # Generate the power report body
    report_body = "Failed to generate report"
    try:
        report_body = generate_power_report(feeder, today_date, is_today=True)
    except Exception as rep_err:
        logger.error(f"Failed to generate report in test_power_email: {rep_err}")
        
    # Send power email Celery task
    try:
        celery_app.send_task(
            "myapp.tasks.send_power_email", 
            args=[feeder.name, status, device_time, server_time, contact_phone]
        )
    except Exception as e:
        logger.error(f"Failed to enqueue test power email: {e}")
        
    # Send WhatsApp message
    whatsapp_status = "Failed"
    try:
        res = send_whatsapp_power_message(contact_phone, report_body)
        if res:
            whatsapp_status = "Sent"
    except Exception as wa_err:
        logger.error(f"Failed to send test WhatsApp message: {wa_err}")

    return {
        "status": "Success",
        "message": f"Test power email task for {feeder.name} sent to Celery queue",
        "whatsapp_status": whatsapp_status,
        "report_generated": report_body,
        "server_time": server_time
    }

@app.get("/api/test-daily-power-updates/")
async def test_daily_power_updates():
    logger.info("Test daily power updates endpoint called")
    
    lagos_tz = timezone(timedelta(hours=1))
    yesterday = (datetime.now(lagos_tz) - timedelta(days=1)).date()
    
    feeders = []
    try:
        with engine.connect() as conn:
            rows = conn.execute(text("SELECT id, name, registered_phone, band FROM myapp_feeder")).fetchall()
            for r in rows:
                feeders.append(FeederObj(r[0], r[1], r[2], r[3]))
    except Exception as e:
        logger.error(f"Error fetching Feeders for test_daily_power_updates: {e}", exc_info=True)
        
    reports_sent = []
    
    for feeder in feeders:
        try:
            report_body = generate_power_report(feeder, yesterday, is_today=False)
            phone_to_use = feeder.contact_phone
            whatsapp_status = "Skipped (No phone)"
            if phone_to_use:
                try:
                    res = send_whatsapp_power_message(phone_to_use, report_body)
                    if res:
                        whatsapp_status = "Sent"
                    else:
                        whatsapp_status = "Failed"
                except Exception as wa_err:
                    whatsapp_status = f"Error: {wa_err}"
            
            reports_sent.append({
                "feeder_name": feeder.name,
                "phone": phone_to_use,
                "whatsapp_status": whatsapp_status,
                "report_preview": report_body[:100] + "..." if len(report_body) > 100 else report_body
            })
        except Exception as err:
            reports_sent.append({
                "feeder_name": feeder.name,
                "error": str(err)
            })
            
    # Trigger the Celery task to run completely in the background
    try:
        celery_app.send_task("myapp.tasks.send_daily_power_updates")
    except Exception as e:
        logger.error(f"Failed to enqueue test daily power updates: {e}")
        
    return {
        "status": "Success",
        "message": "Test daily power updates task sent to Celery queue",
        "reports_processed": reports_sent
    }


@app.get("/robots.txt")
async def robots():
    return PlainTextResponse("User-agent: *\nDisallow:")

@app.get("/favicon.ico")
async def favicon():
    return Response(status_code=204)



