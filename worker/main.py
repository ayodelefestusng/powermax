from datetime import datetime, timezone, timedelta
import logging
from fastapi import FastAPI, HTTPException, Request, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from sqlalchemy import text

from worker.celery_app import celery_app
from worker.db import engine

# Logger configuration
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("WorkerGateway")

app = FastAPI(title="FastAPI Worker Gateway API")

class PowerStatus(BaseModel):
    status: str
    timestamp: int
    peak_a0: int
    feeder_name: str
    transformer_name: str
    contact_phone: str
    msisdn: str

def save_power_status_update(data: PowerStatus, server_time_dt):
    lagos_tz = timezone(timedelta(hours=1))
    now_local = datetime.now(lagos_tz)

    try:
        with engine.begin() as conn:
            # Check if feeder exists
            feeder_query = text("SELECT id, transformer_name, contact_phone, msisdn FROM myapp_feeder WHERE name = :name")
            feeder = conn.execute(feeder_query, {"name": data.feeder_name}).fetchone()
            
            if not feeder:
                # Create feeder
                insert_feeder_query = text("""
                    INSERT INTO myapp_feeder (name, transformer_name, contact_phone, msisdn, created_at)
                    VALUES (:name, :transformer_name, :contact_phone, :msisdn, :created_at)
                    RETURNING id
                """)
                feeder_id = conn.execute(insert_feeder_query, {
                    "name": data.feeder_name,
                    "transformer_name": data.transformer_name,
                    "contact_phone": data.contact_phone,
                    "msisdn": data.msisdn,
                    "created_at": now_local
                }).scalar()
            else:
                feeder_id = feeder[0]
                # Update feeder fields if they changed
                if feeder[1] != data.transformer_name or feeder[2] != data.contact_phone or feeder[3] != data.msisdn:
                    update_feeder_query = text("""
                        UPDATE myapp_feeder
                        SET transformer_name = :transformer_name, contact_phone = :contact_phone, msisdn = :msisdn
                        WHERE id = :id
                    """)
                    conn.execute(update_feeder_query, {
                        "transformer_name": data.transformer_name,
                        "contact_phone": data.contact_phone,
                        "msisdn": data.msisdn,
                        "id": feeder_id
                    })
            
            # Save power status
            insert_status_query = text("""
                INSERT INTO myapp_powerstatus (feeder_id, status, timestamp, peak_a0, server_time, contact_phone, msisdn)
                VALUES (:feeder_id, :status, :timestamp, :peak_a0, :server_time, :contact_phone, :msisdn)
            """)
            conn.execute(insert_status_query, {
                "feeder_id": feeder_id,
                "status": data.status.upper(),
                "timestamp": data.timestamp,
                "peak_a0": data.peak_a0,
                "server_time": server_time_dt,
                "contact_phone": data.contact_phone,
                "msisdn": data.msisdn
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

@app.post("/power-tracker-gateway/")
async def power_update1(data: PowerStatus, request: Request):
    lagos_tz = timezone(timedelta(hours=1))
    server_time_dt = datetime.now(lagos_tz)
    server_time = server_time_dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    
    logger.info(
        f"Incoming alert from Feeder: {data.feeder_name} [{data.transformer_name}] "
        f"-> Status: {data.status.upper()} (SIM ID: {data.msisdn})"
    )

    try:
        # Check if the hardware SIM identity matches the designated contact phone
        if data.msisdn != "UNKNOWN" and data.msisdn.strip() != data.contact_phone.strip():
            logger.warning(
                f"SECURITY MATCH MISMATCH DETECTED: Node {data.transformer_name} reported SIM ID {data.msisdn} "
                f"but expects Contact Profile Phone {data.contact_phone}!"
            )
            try:
                celery_app.send_task(
                    "myapp.tasks.send_security_alert_email",
                    args=[
                        data.feeder_name,
                        data.transformer_name,
                        data.contact_phone,
                        data.msisdn,
                        server_time
                    ]
                )
                logger.info("Security mismatch notification handed off to Celery workers.")
            except Exception as celery_sec_err:
                logger.error(f"Failed to offload security task to Celery: {celery_sec_err}")

        # Persist feeder update in the database
        try:
            save_power_status_update(data, server_time_dt)
        except Exception as db_err:
            logger.error(f"Database persistence failed: {db_err}")

        # Offload the standard grid event tracking pipeline to the core workers
        try:
            celery_app.send_task(
                "myapp.tasks.send_power_email", 
                args=[data.feeder_name, data.status, data.timestamp, server_time, data.contact_phone]
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
        logger.error(f"Critical application processing breakdown within gateway route context: {e}")
        return JSONResponse(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            content={"status": "error", "message": "Internal streaming metadata exception fault"}
        )
