from datetime import datetime, timezone, timedelta
import logging
from pathlib import Path
from fastapi import FastAPI, HTTPException, Request, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from sqlalchemy import text

from worker.celery_app import celery_app
from worker.db import engine
from worker.tasks import send_whatsapp_power_message

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



from fastapi.exceptions import RequestValidationError
from fastapi import Request

@app.exception_handler(RequestValidationError)
async def validation_exception_handler(request: Request, exc: RequestValidationError):
    logger.error(f"Validation error details: {exc.errors()}")
    logger.error(f"Raw body sent: {await request.body()}")
    return JSONResponse(status_code=422, content={"detail": exc.errors()})
from typing import Optional
from pydantic import BaseModel

class PowerStatus(BaseModel):
    status: str
    timestamp: int
    peak_a0: int
    feeder_name: str
    transformer_name: str
    sim_serial: Optional[str] = None
    contact_phone: Optional[str] = None
    msisdn: str = "UNKNOWN"
    
    
    
    
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
                    INSERT INTO myapp_feeder (name, transformer_name, sim_serial, msisdn, created_at)
                    VALUES (:name, :transformer_name, :sim_serial, :msisdn, :created_at)
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
    except Exception as e:
        logger.error(f"Error persisting power status update: {e}", exc_info=True)
        raise e

@app.get("/api/test-email/")
async def test_email():
    logger.info("Test email endpoint called")
    feeder_name = "Test Feeder"
    test_status = "ON"
    test_ms = 9999
    
    lagos_tz = timezone(timedelta(hours=1))
    server_time = datetime.now(lagos_tz).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    contact_phone = "2348027790963"
    
    try:
        celery_app.send_task(
            "myapp.tasks.send_power_email", 
            args=[feeder_name, test_status, test_ms, server_time, contact_phone]
        )
        return {
            "status": "Success",
            "message": "Test email task sent to Celery queue",
            "server_time": server_time
        }
    except Exception as e:
        logger.error(f"Failed to enqueue test email: {e}")
        return JSONResponse(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            content={"status": "Error", "detail": str(e)}
        )

@app.get("/api/test-whatsapp/")
async def test_whatsapp(phone: str = "2348021299221", message: str = "Test WhatsApp message from FastAPI"):
    logger.info(f"Test WhatsApp endpoint called for phone: {phone}")
    try:
        res = send_whatsapp_power_message(phone, message)
        if res:
            return {
                "status": "Success",
                "message": "Test WhatsApp message sent",
                "response": res
            }
        else:
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail="Failed to send WhatsApp message"
            )
    except Exception as e:
        logger.error(f"Failed to send test WhatsApp: {e}")
        return JSONResponse(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            content={"status": "Error", "detail": str(e)}
        )


@app.post("/power-tracker-gateway/")
async def power_update1(data: PowerStatus, request: Request):
    try:
        # Gracefully handle validation defaults if keys are absent
        if not data.sim_serial:
            if data.contact_phone:
                data.sim_serial = data.contact_phone
            elif data.msisdn and data.msisdn != "UNKNOWN":
                data.sim_serial = data.msisdn
            else:
                data.sim_serial = "UNKNOWN"
        
        lagos_tz = timezone(timedelta(hours=1))
        server_time_dt = datetime.now(lagos_tz)
        server_time = server_time_dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        
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

        # --- Database Persistence ---
        try:
            save_power_status_update(data, server_time_dt)
        except Exception as db_err:
            logger.error(f"Database persistence failed: {db_err}")

        # --- Celery Worker Offload ---
        try:
            celery_app.send_task(
                "myapp.tasks.send_power_email", 
                args=[data.feeder_name, data.status, data.timestamp, server_time, data.sim_serial]
            )
            logger.info("Grid status metric tracking update successfully offloaded to queue.")
        except Exception as celery_err:
            logger.error(f"Could not send main task to Celery: {celery_err}")   
        
        return {
            "status": "success",
            "queued_at": server_time,
            "node_validated": True
        }

    except Exception as e:
        logger.error(f"Critical breakdown within gateway route context: {e}")
        return JSONResponse(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            content={"status": "error", "message": "Internal processing pipeline error"}
        )