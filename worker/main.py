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
from fastapi import FastAPI, Request, status, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field, ConfigDict
from datetime import datetime, timezone, timedelta
from typing import Optional
import json

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
from pydantic import BaseModel, Field, ConfigDict, ValidationError


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
    status: str = Field(..., alias="stat")
    timestamp: Optional[int] = Field(default=0) 
    peak_a0: int = Field(..., alias="val")
    feeder_name: str = Field(..., alias="fdr")
    transformer_code: str = Field(default="UNKNOWN_TRANSFORMER", alias="tf")
    sim_serial: Optional[str] = Field(default="UNKNOWN", alias="ccid")
    contact_phone: Optional[str] = None
    msisdn: str = "UNKNOWN"

    model_config = ConfigDict(populate_by_name=True)          



@app.post("/power-tracker-gateway/")
async def power_update(request: Request):
    # Force immediate connection termination headers for the SIM900
    headers = {"Connection": "close", "Content-Type": "application/json"}
    
    try:
        # 1. Read raw incoming body bytes directly to bypass any framework hang
        body_bytes = await request.body()
        body_str = body_bytes.decode("utf-8").strip()
        
        if not body_str:
            raise ValueError("Empty body stream received")

        # 2. Parse raw json dictionary directly (Bypasses Pydantic completely)
        payload = json.loads(body_str)
        
        # 3. Extract values using inline alias fallbacks
        status_val = payload.get("stat") or payload.get("status")
        peak_val   = payload.get("val")  or payload.get("peak_a0")
        feeder     = payload.get("fdr")  or payload.get("feeder_name")
        xfrmr      = payload.get("tf")   or payload.get("transformer_name", "UNKNOWN_TRANSFORMER")
        serial     = payload.get("ccid") or payload.get("sim_serial", "UNKNOWN")
        msisdn     = payload.get("msisdn", "UNKNOWN")
        timestamp  = payload.get("timestamp", 0)

        # Log the raw payload for deep visibility
        logger.info(f"PowerMonitor: Raw body received successfully: {body_str}")

        if not status_val or peak_val is None or not feeder:
            logger.error(f"Ingest rejected - Missing critical keys. Payload: {payload}")
            return JSONResponse(
                status_code=status.HTTP_200_OK, 
                headers=headers,
                content={"status": "rejected", "message": "Missing core tracking parameters"}
            )

        # 4. Handle timing metrics
        lagos_tz = timezone(timedelta(hours=1))
        server_time_dt = datetime.now(lagos_tz)
        server_time = server_time_dt.strftime("%Y-%m-%d %H:%M:%S") + f".{int(server_time_dt.microsecond / 1000):03d}"
        
        logger.info(
            f"on {lagos_tz} Edge Telemetry Successfully Decoded -> Feeder: {feeder} [{xfrmr}] "
            f"-> Status: {str(status_val).upper()} | Peak A0: {peak_val}"
        )

        # --- Direct Celery Worker Offload Pipeline ---
        try:
            celery_app.send_task(
                "myapp.tasks.send_power_email", 
                args=[
                    feeder, 
                    status_val, 
                    timestamp, 
                    server_time, 
                    serial,
                    xfrmr,
                    int(peak_val),
                    msisdn,
                    serial
                ]
            )
            logger.info("Grid status metric tracking update successfully offloaded to queue.")
        except Exception as celery_err:
            logger.error(f"Could not send main task to Celery: {celery_err}")   
        
        return JSONResponse(
            status_code=status.HTTP_200_OK,
            headers=headers,
            content={"status": "success", "queued_at": server_time, "node_validated": True}
        )

    except Exception as e:
        logger.error(f"Critical breakdown within gateway route context: {e}")
        return JSONResponse(
            status_code=status.HTTP_200_OK, 
            headers=headers,
            content={"status": "error", "message": str(e)}
        )
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
            feeder_query = text("SELECT id, transformer_name, sim_serial, msisdn, transformer_code FROM myapp_feeder WHERE name = :name")
            feeder = conn.execute(feeder_query, {"name": data.feeder_name}).fetchone()
            
            # Look up Feeder.transformer_name using transformer_code
            resolved_transformer_name = "UNKNOWN_TRANSFORMER"
            if data.transformer_code and data.transformer_code != "UNKNOWN_TRANSFORMER":
                lookup_query = text("SELECT transformer_name FROM myapp_feeder WHERE transformer_code = :code LIMIT 1")
                lookup_res = conn.execute(lookup_query, {"code": data.transformer_code}).fetchone()
                if lookup_res and lookup_res[0]:
                    resolved_transformer_name = lookup_res[0]
                else:
                    resolved_transformer_name = data.transformer_code
            else:
                resolved_transformer_name = data.transformer_code

            if not feeder:
                # Create feeder
                insert_feeder_query = text("""
                    INSERT INTO myapp_feeder (name, transformer_name, transformer_code, sim_serial, msisdn, band, created_at)
                    VALUES (:name, :transformer_name, :transformer_code, :sim_serial, :msisdn, 'A', :created_at)
                    RETURNING id
                """)
                feeder_id = conn.execute(insert_feeder_query, {
                    "name": data.feeder_name,
                    "transformer_name": resolved_transformer_name,
                    "transformer_code": data.transformer_code,
                    "sim_serial": data.sim_serial,
                    "msisdn": data.msisdn,
                    "created_at": now_local
                }).scalar()
            else:
                feeder_id = feeder[0]
                # Update feeder fields if they changed
                if feeder[1] != resolved_transformer_name or feeder[2] != data.sim_serial or feeder[3] != data.msisdn or feeder[4] != data.transformer_code:
                    update_feeder_query = text("""
                        UPDATE myapp_feeder
                        SET transformer_name = :transformer_name, transformer_code = :transformer_code, sim_serial = :sim_serial, msisdn = :msisdn
                        WHERE id = :id
                    """)
                    conn.execute(update_feeder_query, {
                        "transformer_name": resolved_transformer_name,
                        "transformer_code": data.transformer_code,
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



@app.get("/api/test-email245/")
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



#Utility Endpoints   

@app.get("/utility/")
def read_root():
    return {"message": "Hello from SIM 900"}

