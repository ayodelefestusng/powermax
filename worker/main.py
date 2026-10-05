from datetime import datetime, timezone, timedelta
import logging
from pathlib import Path
import uuid
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
    event_id: Optional[str] = None

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

        # 4. Handle timing metrics & timestamp adoption
        server_time_dt = datetime.now(lagos_tz)
        server_time = server_time_dt.strftime("%Y-%m-%d %H:%M:%S") + f".{int(server_time_dt.microsecond / 1000):03d}"
        
        use_server_time = False
        adopted_time_dt = server_time_dt
        try:
            raw_ts = float(timestamp) if timestamp is not None else 0.0
            if raw_ts <= 0:
                use_server_time = True
            else:
                if raw_ts > 1e11:  # epoch in milliseconds
                    raw_ts /= 1000.0
                payload_time_dt = datetime.fromtimestamp(raw_ts, tz=lagos_tz)
                drift_seconds = abs((server_time_dt - payload_time_dt).total_seconds())
                if drift_seconds > 8 * 3600:
                    use_server_time = True
                    logger.warning(
                        f"Timestamp drift ({drift_seconds/3600:.2f}h) exceeds 8-hour threshold. "
                        f"Adopting server time instead of payload timestamp {timestamp}."
                    )
                else:
                    adopted_time_dt = payload_time_dt
        except (ValueError, TypeError, OverflowError, OSError) as ts_err:
            logger.warning(f"Could not parse payload timestamp '{timestamp}': {ts_err}. Adopting server time.")
            use_server_time = True

        if use_server_time:
            effective_timestamp = int(server_time_dt.timestamp())
            human_timestamp = server_time_dt.strftime("%Y-%m-%d %H:%M:%S")
        else:
            effective_timestamp = int(adopted_time_dt.timestamp())
            human_timestamp = adopted_time_dt.strftime("%Y-%m-%d %H:%M:%S")

        # Generate unique event_id for this telemetry event (FastAPI generates it as hardware doesn't)
        event_id = str(uuid.uuid4())

        now_str = datetime.now(lagos_tz).strftime("%Y-%m-%d %H:%M:%S")

        # Log the raw payload for deep visibility
        logger.info(f"Time Received {now_str} : PowerMonitor: Raw body received: {body_str}")
        logger.info(f"Time Stamp {now_str} : PowerMonitor: Adopted timestamp: {human_timestamp} (raw: {timestamp}, effective: {effective_timestamp}, event_id: {event_id})")
        if not status_val or peak_val is None or not feeder:
            logger.error(f"Ingest rejected - Missing critical keys. Payload: {payload}")
            return JSONResponse(
                status_code=status.HTTP_200_OK, 
                headers=headers,
                content={"status": "rejected", "message": "Missing core tracking parameters"}
            )
        
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
                    effective_timestamp, 
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
                    event_id,
                ]
            )
            logger.info(f"Grid status metric tracking update successfully offloaded to queue with event_id={event_id}.")
        except Exception as celery_err:
            logger.error(f"Could not send main task to Celery: {celery_err}")   
        
        return JSONResponse(
            status_code=status.HTTP_200_OK,
            headers=headers,
            content={"status": "success", "event_id": event_id, "queued_at": server_time, "node_validated": True}
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
                             stat_b=None, volt_b: float = 0.0,
                             event_id: str = None):
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
                    "whatsapp_primary": "2348021299221, 2349068770054",
                    "whatsapp_group": "120363410539285836@g.us, 120363429032532411@g.us, 120363429460546485@g.us",
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
            
            # Check previous power status for this feeder to detect actual state change
            prev_status_query = text("""
                SELECT id, status, stat_r, stat_y, stat_b, server_time 
                FROM myapp_powerstatus 
                WHERE feeder_id = :feeder_id 
                ORDER BY server_time DESC 
                LIMIT 2
            """)
            prev_records = conn.execute(prev_status_query, {"feeder_id": feeder_id}).fetchall()
            prev_record = prev_records[0] if prev_records else None
            prev_prev_record = prev_records[1] if len(prev_records) > 1 else None

            is_pearl = (str(dt).upper() == "PEARL")
            new_status_str = (data.status or "").strip().upper()
            
            if prev_record is None:
                status_changed = True
            else:
                prev_status_str = (prev_record[1] or "").strip().upper()
                if is_pearl and stat_r is not None:
                    prev_r = (prev_record[2] or "").strip().upper()
                    prev_y = (prev_record[3] or "").strip().upper()
                    prev_b = (prev_record[4] or "").strip().upper()
                    cur_r = (str(stat_r) or "").strip().upper()
                    cur_y = (str(stat_y) or "").strip().upper()
                    cur_b = (str(stat_b) or "").strip().upper()
                    status_changed = (
                        new_status_str != prev_status_str
                        or cur_r != prev_r
                        or cur_y != prev_y
                        or cur_b != prev_b
                    )
                else:
                    status_changed = (new_status_str != prev_status_str)

                # Filter out 0-minute bounce / transient spike (< 60 seconds reverting to previous state)
                if status_changed and prev_prev_record and prev_record[5]:
                    prev_prev_status_str = (prev_prev_record[1] or "").strip().upper()
                    if new_status_str == prev_prev_status_str:
                        prev_time = prev_record[5]
                        curr_time = server_time_dt
                        if prev_time.tzinfo is None and curr_time.tzinfo is not None:
                            prev_time = curr_time.tzinfo.localize(prev_time)
                        elif prev_time.tzinfo is not None and curr_time.tzinfo is None:
                            curr_time = prev_time.tzinfo.localize(curr_time)
                        
                        duration_diff = (curr_time - prev_time).total_seconds()
                        if 0 <= duration_diff < 60:
                            logger.info(
                                f"Detected 0-min transient blip ({duration_diff:.1f}s) for feeder {data.feeder_name}. "
                                f"Cleaning up transient record {prev_record[0]} and suppressing alert."
                            )
                            conn.execute(text("DELETE FROM myapp_powerstatus WHERE id = :id"), {"id": prev_record[0]})
                            return feeder_id, False, None

            # Guardrail: If current status in DB is same as payload status, ignore and don't update DB
            if not status_changed:
                logger.info(
                    f"Guardrail triggered: Status for Feeder {data.feeder_name} is unchanged ({new_status_str}). "
                    f"Ignoring payload and skipping database insertion."
                )
                return feeder_id, False, None

            # Resolve effective event_id
            effective_event_id = str(event_id or data.event_id or uuid.uuid4())

            # Save power status — includes event_id, whatsapp_status='undelivered', and phase columns for PEARL DT
            insert_status_query = text("""
                INSERT INTO myapp_powerstatus (
                    event_id, whatsapp_status,
                    feeder_id, status, timestamp, peak_a0, server_time,
                    sim_serial, msisdn,
                    dt, volt_r, stat_r, volt_y, stat_y, volt_b, stat_b
                )
                VALUES (
                    :event_id, 'undelivered',
                    :feeder_id, :status, :timestamp, :peak_a0, :server_time,
                    :sim_serial, :msisdn,
                    :dt, :volt_r, :stat_r, :volt_y, :stat_y, :volt_b, :stat_b
                )
            """)
            conn.execute(insert_status_query, {
                "event_id": effective_event_id,
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
            logger.info(f"Persisted power status update in database for feeder {data.feeder_name} [event_id={effective_event_id}, dt={dt}, changed=True]")
            return feeder_id, True, effective_event_id
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
            target_instance = evolution_instance.strip() if (evolution_instance and evolution_instance.strip() not in ("", "Hhh")) else os.getenv("POWER_INSTANCE", "feeder_tracking")
            if guardian_phone and target_instance:
                if status_lower == 'in':
                    message = f"{student_name} has arrived school"
                else:
                    time_str = now_local.strftime('%I:%M%p').lower().lstrip('0')
                    message = f"{student_name} has left school at {time_str}"
                
                # Resolve API URL and Key
                api_url = os.getenv("EVOLUTION_API_URL", "https://vectra-evolution-api2.qgmg5v.easypanel.host")
                api_key = os.getenv("EVOLUTION_API_KEY", "4296843w3C4wwC977eeerr415CAwwed")
                
                if tenant_evolution_api and tenant_evolution_api.strip() not in ("", "Hhh"):
                    val = tenant_evolution_api.strip()
                    if val.startswith("http://") or val.startswith("https://"):
                        api_url = val
                    else:
                        api_key = val
                
                try:
                    celery_app.send_task(
                        "myapp.tasks.send_attendance_whatsapp",
                        args=[api_url, api_key, target_instance, guardian_phone, message]
                    )
                    logger.info(f"Enqueued Celery task send_attendance_whatsapp for guardian of {student_name} (Phone: {guardian_phone}, Instance: {target_instance})")
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
    return {"message": "Hello from SIM 900 20082026v2 timestap 05102026"}


@app.api_route("/feeder_lookup", methods=["GET", "POST"])
@app.api_route("/feeder_lookup/", methods=["GET", "POST"])
async def feeder_lookup(
    request: Request,
    meter_number: Optional[str] = None,
    meter: Optional[str] = None,
    account_number: Optional[str] = None
):
    """
    Look up feeder and band information from Ikeja Electric customer feeder verification.
    Accepts meter/account number via GET query parameter (?meter_number=...) or POST JSON payload.
    """
    input_meter = meter_number or meter or account_number

    if request.method == "POST":
        try:
            body = await request.json()
            if isinstance(body, dict):
                input_meter = (
                    body.get("meter_number")
                    or body.get("meter")
                    or body.get("account_number")
                    or body.get("account")
                    or input_meter
                )
        except Exception:
            pass

    if not input_meter or not str(input_meter).strip():
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Missing meter_number or account_number parameter"
        )

    clean_meter = str(input_meter).strip()
    logger.info(f"feeder_lookup requested for meter/account: {clean_meter}")

    powerbi_resource_key = "f8264200-5ceb-480f-996f-c9b09e656b97"
    powerbi_model_id = 1579241
    powerbi_report_id = 1977301
    powerbi_cluster_api = "https://wabi-north-europe-l-primary-api.analysis.windows.net"

    import uuid
    url = f"{powerbi_cluster_api}/public/reports/querydata?synchronous=true"
    headers = {
        "Accept": "application/json",
        "ActivityId": str(uuid.uuid4()),
        "RequestId": str(uuid.uuid4()),
        "X-PowerBI-ResourceKey": powerbi_resource_key,
        "Content-Type": "application/json;charset=UTF-8",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    }

    query_payload = {
        "version": "1.0.0",
        "queries": [
            {
                "Query": {
                    "Commands": [
                        {
                            "SemanticQueryDataShapeCommand": {
                                "Query": {
                                    "Version": 2,
                                    "From": [{"Name": "a", "Entity": "All", "Type": 0}],
                                    "Select": [
                                        {
                                            "Column": {
                                                "Expression": {"SourceRef": {"Source": "a"}},
                                                "Property": "ACCOUNT_NUMBER"
                                            },
                                            "Name": "All.ACCOUNT_NUMBER"
                                        },
                                        {
                                            "Column": {
                                                "Expression": {"SourceRef": {"Source": "a"}},
                                                "Property": "NAME_OF_FEEDER"
                                            },
                                            "Name": "All.NAME_OF_FEEDER"
                                        },
                                        {
                                            "Column": {
                                                "Expression": {"SourceRef": {"Source": "a"}},
                                                "Property": "FEEDER_BAND"
                                            },
                                            "Name": "All.FEEDER_BAND"
                                        }
                                    ],
                                    "Where": [
                                        {
                                            "Condition": {
                                                "Contains": {
                                                    "Left": {
                                                        "Column": {
                                                            "Expression": {"SourceRef": {"Source": "a"}},
                                                            "Property": "ACCOUNT_NUMBER"
                                                        }
                                                    },
                                                    "Right": {
                                                        "Literal": {
                                                            "Value": f"'{clean_meter}'"
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    ]
                                },
                                "Binding": {
                                    "Primary": {
                                        "Groupings": [{"Projections": [0, 1, 2]}]
                                    },
                                    "DataReduction": {
                                        "DataVolume": 3,
                                        "Primary": {"Window": {"Count": 10}}
                                    },
                                    "Version": 1
                                },
                                "ExecutionMetricsKind": 1
                            }
                        }
                    ]
                },
                "QueryId": "",
                "ApplicationContext": {
                    "DatasetId": str(powerbi_model_id),
                    "Sources": [{"ReportId": str(powerbi_report_id)}]
                }
            }
        ],
        "cancelQueries": [],
        "modelId": powerbi_model_id
    }

    try:
        response = requests.post(url, headers=headers, json=query_payload, timeout=20)
        if response.status_code != 200:
            logger.error(f"PowerBI API returned status {response.status_code}: {response.text[:200]}")
            return JSONResponse(
                status_code=status.HTTP_502_BAD_GATEWAY,
                content={
                    "status": "error",
                    "message": f"Failed to fetch data from Ikeja Electric verification service (Status {response.status_code})"
                }
            )

        data = response.json()
        results = data.get("results", [])
        if not results:
            return JSONResponse(
                status_code=status.HTTP_404_NOT_FOUND,
                content={"status": "not_found", "message": f"No feeder details found for '{clean_meter}'"}
            )

        dsr = results[0].get("result", {}).get("data", {}).get("dsr", {})
        ds_list = dsr.get("DS", [])
        if not ds_list:
            return JSONResponse(
                status_code=status.HTTP_404_NOT_FOUND,
                content={"status": "not_found", "message": f"No feeder details found for '{clean_meter}'"}
            )

        ds = ds_list[0]
        value_dicts = ds.get("ValueDicts", {})
        d0 = value_dicts.get("D0", [])
        d1 = value_dicts.get("D1", [])
        d2 = value_dicts.get("D2", [])

        ph = ds.get("PH", [])
        records = []
        last_row = [None, None, None]

        for p in ph:
            dm0 = p.get("DM0", [])
            for item in dm0:
                c = item.get("C", [])
                r_mask = item.get("R", 0)

                cur_row = list(last_row)
                c_idx = 0
                for col_i in range(3):
                    is_repeated = bool(r_mask & (1 << (2 - col_i))) if r_mask else False
                    if not is_repeated:
                        if c_idx < len(c):
                            val = c[c_idx]
                            c_idx += 1
                            if col_i == 0:
                                cur_row[0] = d0[val] if isinstance(val, int) and val < len(d0) else str(val)
                            elif col_i == 1:
                                cur_row[1] = d1[val] if isinstance(val, int) and val < len(d1) else str(val)
                            elif col_i == 2:
                                cur_row[2] = d2[val] if isinstance(val, int) and val < len(d2) else str(val)

                last_row = list(cur_row)
                if cur_row[0] or cur_row[1]:
                    records.append({
                        "account_number": cur_row[0],
                        "feeder": cur_row[1],
                        "band": cur_row[2]
                    })

        if not records:
            return JSONResponse(
                status_code=status.HTTP_404_NOT_FOUND,
                content={
                    "status": "not_found",
                    "meter_number": clean_meter,
                    "message": f"No feeder details found for meter/account '{clean_meter}'"
                }
            )

        primary = records[0]
        return {
            "status": "success",
            "meter_number": clean_meter,
            "account_number": primary.get("account_number"),
            "feeder": primary.get("feeder"),
            "band": primary.get("band"),
            "matches": records
        }

    except requests.exceptions.Timeout:
        logger.error(f"Timeout querying Ikeja Electric feeder for meter {clean_meter}")
        return JSONResponse(
            status_code=status.HTTP_504_GATEWAY_TIMEOUT,
            content={"status": "error", "message": "Request to Ikeja Electric verification service timed out"}
        )
    except Exception as e:
        logger.error(f"Error querying Ikeja Electric feeder: {e}", exc_info=True)
        return JSONResponse(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            content={"status": "error", "message": str(e)}
        )


