from datetime import datetime, timezone, timedelta
import logging
from pathlib import Path
from fastapi import FastAPI, HTTPException, Request, status, BackgroundTasks
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
    # Force immediate connection termination headers for the SIM900/ESP32
    headers = {"Connection": "close", "Content-Type": "application/json"}
    lagos_tz = timezone(timedelta(hours=1))
    try:
        # 1. Read raw incoming body bytes directly to bypass any framework hang
        body_bytes = await request.body()
        body_str = body_bytes.decode("utf-8").strip()
        
        if not body_str:
            logger.error(f"Time Received {lagos_tz} : Ingest rejected - Empty body stream received. Raw body: {body_bytes}")
            raise ValueError("Empty body stream received")

        # 2. Parse raw json dictionary directly (Bypasses Pydantic completely)
        payload = json.loads(body_str)
        
        # 3. Extract values using inline alias fallbacks (checking key existence / non-None to allow 0.0 or 0)
        status_val = payload.get("stat") if payload.get("stat") is not None else payload.get("status")
        peak_val   = payload.get("val") if payload.get("val") is not None else payload.get("peak_a0")
        feeder     = payload.get("fdr")  or payload.get("feeder_name")
        xfrmr      = payload.get("tf")   or payload.get("transformer_name", "UNKNOWN_TRANSFORMER")
        serial     = payload.get("ccid") or payload.get("sim_serial", "UNKNOWN")
        msisdn     = payload.get("msisdn", "UNKNOWN")
        timestamp  = payload.get("timestamp", 0)
        contact_phone = payload.get("contact_phone")
        dt_code    = payload.get("dt", "")  # Device type identifier e.g. "PEARL"

        # 3a. Three-phase fields (populated by PEARL DT and future 3-phase nodes)
        stat_r = payload.get("stat_r", None)   # "ON" / "OFF"
        volt_r = payload.get("volt_r", 0.0)
        stat_y = payload.get("stat_y", None)
        volt_y = payload.get("volt_y", 0.0)
        stat_b = payload.get("stat_b", None)
        volt_b = payload.get("volt_b", 0.0)

        
        # Convert timestamp to human-readable date/time
        human_timestamp = str(timestamp)
        if timestamp:
            try:
                ts_val = float(timestamp)
                if ts_val > 1e11:  # epoch in milliseconds
                    ts_val /= 1000.0
                if ts_val > 0:
                    human_timestamp = datetime.fromtimestamp(ts_val, tz=lagos_tz).strftime("%Y-%m-%d %H:%M:%S")
            except (ValueError, TypeError, OverflowError):
                human_timestamp = str(timestamp)

        now_str = datetime.now(lagos_tz).strftime("%Y-%m-%d %H:%M:%S")

        # Log the raw payload for deep visibility
        logger.info(f"Time Received {now_str} : PowerMonitor: Raw body received: {body_str}")
        logger.info(f"Time Stamp {now_str} : PowerMonitor: Timestamp: {human_timestamp} (raw: {timestamp})")
        if not status_val or peak_val is None or not feeder:
            logger.error(f"Ingest rejected - Missing critical keys. Payload: {payload}")
            return JSONResponse(
                status_code=status.HTTP_200_OK, 
                headers=headers,
                content={"status": "rejected", "message": "Missing core tracking parameters"}
            )

        # 4. Handle timing metrics
        server_time_dt = datetime.now(lagos_tz)
        server_time = server_time_dt.strftime("%Y-%m-%d %H:%M:%S") + f".{int(server_time_dt.microsecond / 1000):03d}"
        
        is_pearl = (dt_code.upper() == "PEARL")
        if is_pearl:
            logger.info(
                f"[PEARL DT] Three-phase telemetry → Feeder: {feeder} [{xfrmr}] "
                f"R:{stat_r}/{volt_r}V  Y:{stat_y}/{volt_y}V  B:{stat_b}/{volt_b}V  "
                f"Combined:{str(status_val).upper()}"
            )
        else:
            logger.info(
                f"Edge Telemetry Decoded → Feeder: {feeder} [{xfrmr}] "
                f"Status: {str(status_val).upper()} | Peak A0: {peak_val}"
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
                    contact_phone,
                    xfrmr,
                    int(peak_val),
                    msisdn,
                    serial,
                    dt_code,
                    stat_r,
                    float(volt_r),
                    stat_y,
                    float(volt_y),
                    stat_b,
                    float(volt_b),
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
def save_power_status_update(data: PowerStatus, server_time_dt,
                             dt: str = "",
                             stat_r=None, volt_r: float = 0.0,
                             stat_y=None, volt_y: float = 0.0,
                             stat_b=None, volt_b: float = 0.0):
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
                # Create feeder with default WhatsApp recipients
                insert_feeder_query = text("""
                    INSERT INTO myapp_feeder (
                        name, transformer_name, transformer_code, sim_serial, msisdn,
                        band, created_at, whatsapp_primary, whatsapp_group
                    )
                    VALUES (
                        :name, :transformer_name, :transformer_code, :sim_serial, :msisdn,
                        'A', :created_at, :whatsapp_primary, :whatsapp_group
                    )
                    RETURNING id
                """)
                feeder_id = conn.execute(insert_feeder_query, {
                    "name": data.feeder_name,
                    "transformer_name": resolved_transformer_name,
                    "transformer_code": data.transformer_code,
                    "sim_serial": data.sim_serial,
                    "msisdn": data.msisdn,
                    "created_at": now_local,
                    "whatsapp_primary": "2348021299221, 2348108383472",
                    "whatsapp_group": "120363410539285836@g.us, 120363429032532411@g.us",
                }).scalar()
            else:
                feeder_id = feeder[0]
                # Update feeder fields if they changed
                if feeder[1] != resolved_transformer_name or feeder[2] != data.sim_serial or feeder[3] != data.msisdn or feeder[4] != data.transformer_code:
                    update_feeder_query = text("""
                        UPDATE myapp_feeder
                        SET transformer_name = :transformer_name, transformer_code = :transformer_code,
                            sim_serial = :sim_serial, msisdn = :msisdn
                        WHERE id = :id
                    """)
                    conn.execute(update_feeder_query, {
                        "transformer_name": resolved_transformer_name,
                        "transformer_code": data.transformer_code,
                        "sim_serial": data.sim_serial,
                        "msisdn": data.msisdn,
                        "id": feeder_id
                    })
            
            # Save power status — includes three-phase columns for PEARL DT
            insert_status_query = text("""
                INSERT INTO myapp_powerstatus (
                    feeder_id, status, timestamp, peak_a0, server_time,
                    sim_serial, msisdn,
                    dt, volt_r, stat_r, volt_y, stat_y, volt_b, stat_b
                )
                VALUES (
                    :feeder_id, :status, :timestamp, :peak_a0, :server_time,
                    :sim_serial, :msisdn,
                    :dt, :volt_r, :stat_r, :volt_y, :stat_y, :volt_b, :stat_b
                )
            """)
            conn.execute(insert_status_query, {
                "feeder_id": feeder_id,
                "status": data.status.upper(),
                "timestamp": data.timestamp,
                "peak_a0": data.peak_a0,
                "server_time": server_time_dt,
                "sim_serial": data.sim_serial,
                "msisdn": data.msisdn,
                "dt": dt or "",
                "volt_r": volt_r,
                "stat_r": stat_r,
                "volt_y": volt_y,
                "stat_y": stat_y,
                "volt_b": volt_b,
                "stat_b": stat_b,
            })
            logger.info(f"Persisted power status update in database for feeder {data.feeder_name} [dt={dt}]")
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


import os
import requests

def send_attendance_whatsapp(api_url: str, api_key: str, instance: str, phone: str, message: str):
    url = f"{api_url.rstrip('/')}/message/sendText/{instance}"
    headers = {
        "apikey": api_key,
        "Content-Type": "application/json"
    }
    payload = {
        "number": phone,
        "text": message
    }
    try:
        response = requests.post(url, json=payload, headers=headers, timeout=10)
        if response.status_code in [200, 201]:
            logger.info(f"Attendance WhatsApp notification sent to {phone} successfully.")
        else:
            logger.error(f"Evolution API Error ({response.status_code}): {response.text}")
    except Exception as e:
        logger.error(f"Failed to send attendance WhatsApp notification: {e}", exc_info=True)


class AttendanceRequest(BaseModel):
    tenant_code: str
    student_id: str
    status: str


@app.post("/attendance/")
async def create_attendance(data: AttendanceRequest):
    status_lower = data.status.lower()
    if status_lower not in ['in', 'out']:
        raise HTTPException(status_code=400, detail="status must be 'in' or 'out'")
    
    try:
        with engine.begin() as conn:
            # 1. Look up school tenant by tenant_code
            tenant_query = text("SELECT id, tenant_name, evolution_instance, evolution_api FROM myapp_school_tenant WHERE tenant_code = :tenant_code")
            tenant_row = conn.execute(tenant_query, {"tenant_code": data.tenant_code}).fetchone()
            
            if not tenant_row:
                raise HTTPException(status_code=404, detail=f"School tenant with code '{data.tenant_code}' not found")
                
            tenant_db_id = tenant_row[0]
            evolution_instance = tenant_row[2]
            tenant_evolution_api = tenant_row[3]
            
            # 2. Look up student by student_id and tenant_name_id (the school tenant primary key)
            student_query = text("""
                SELECT id, student_firstname, student_lastname, guardian_phone 
                FROM myapp_student_profile 
                WHERE student_id = :student_id AND tenant_name_id = :tenant_db_id
            """)
            student_row = conn.execute(student_query, {
                "student_id": data.student_id,
                "tenant_db_id": tenant_db_id
            }).fetchone()
            
            if not student_row:
                raise HTTPException(status_code=404, detail=f"Student with id '{data.student_id}' not found under tenant '{data.tenant_code}'")
                
            student_db_id = student_row[0]
            student_firstname = student_row[1]
            student_lastname = student_row[2]
            guardian_phone = student_row[3]
            student_name = f"{student_firstname} {student_lastname}"
            
            # 3. Insert log into myapp_attendance_log
            lagos_tz = timezone(timedelta(hours=1))
            now_local = datetime.now(lagos_tz)
            
            insert_query = text("""
                INSERT INTO myapp_attendance_log (student_id, status, created)
                VALUES (:student_id, :status, :created)
                RETURNING id
            """)
            log_id = conn.execute(insert_query, {
                "student_id": student_db_id,
                "status": status_lower,
                "created": now_local
            }).scalar()
            
            logger.info(f"Recorded attendance for student {data.student_id} under tenant {data.tenant_code}: {status_lower} at {now_local}")
            
            # 4. Asynchronously send WhatsApp message to the guardian_phone if available
            if guardian_phone and evolution_instance:
                if status_lower == 'in':
                    message = f"{student_name} has arrived school"
                else:
                    time_str = now_local.strftime('%I:%M%p').lower().lstrip('0')
                    message = f"{student_name} has left school at {time_str}"
                
                # Resolve API URL and Key
                api_url = os.getenv("EVOLUTION_API_URL", "https://vectra-evolution-api.qgmg5v.easypanel.host")
                api_key = os.getenv("EVOLUTION_API_KEY", "4296843w3C4wwC977eeerr415CAwwed")
                
                if tenant_evolution_api:
                    val = tenant_evolution_api.strip()
                    if val.startswith("http://") or val.startswith("https://"):
                        api_url = val
                    else:
                        api_key = val
                
                try:
                    celery_app.send_task(
                        "myapp.tasks.send_attendance_whatsapp",
                        args=[api_url, api_key, evolution_instance, guardian_phone, message]
                    )
                    logger.info(f"Enqueued Celery task send_attendance_whatsapp for guardian of {student_name} (Phone: {guardian_phone})")
                except Exception as celery_err:
                    logger.error(f"Could not send attendance WhatsApp task to Celery: {celery_err}")
                
            return {"status": "success", "log_id": log_id, "student_id": data.student_id, "tenant_code": data.tenant_code}
            
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Error saving attendance: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=str(e))


#Utility Endpoints   

@app.get("/utility/")
def read_root():
    return {"message": "Hello from SIM 900 17082026v2 timestap"}


@app.api_route("/testing", methods=["GET", "POST"])
async def testing_endpoint(request: Request):
    logger.info("Testing endpoint called")
    headers = {"Connection": "close", "Content-Type": "application/json"}
    return JSONResponse(
        status_code=status.HTTP_200_OK,
        headers=headers,
        content={
            "status": "success",
            "message": "Testing endpoint operational",
            "timestamp": datetime.now(timezone(timedelta(hours=1))).strftime("%Y-%m-%d %H:%M:%S")
        }
    )


