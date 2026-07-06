import os
import smtplib
import logging
import pytz
import requests
from datetime import datetime, time, timedelta
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from sqlalchemy import text

from worker.celery_app import celery_app
from worker.db import engine

# Set up logging
logger = logging.getLogger(__name__)
if not logger.handlers:
    logging.basicConfig(level=logging.INFO)

# Evolution API Credentials
EVOLUTION_API_URL = os.getenv("EVOLUTION_API_URL", "https://vectra-evolution-api.qgmg5v.easypanel.host")
EVOLUTION_API_KEY = os.getenv("EVOLUTION_API_KEY", "")
POWER_INSTANCE = os.getenv("POWER_INSTANCE", "power_max_bot")

class FeederObj:
    def __init__(self, id, name, contact_phone, band=None):
        self.id = id
        self.name = name
        self.contact_phone = contact_phone
        self.band = band

def format_time_colon(dt):
    """
    Format a datetime object to h:mmam/pm in Africa/Lagos timezone (e.g. 5:13am, 12:41pm).
    """
    lagos_tz = pytz.timezone("Africa/Lagos")
    if dt.tzinfo:
        dt = dt.astimezone(lagos_tz)
    else:
        dt = lagos_tz.localize(dt)
    h_12 = dt.strftime("%I")
    minute = dt.strftime("%M")
    ampm = dt.strftime("%p").lower()
    h_12 = str(int(h_12))  # strip leading zero
    return f"{h_12}:{minute}{ampm}"

def format_duration_short(td):
    """
    Format a timedelta object to e.g. 4h 40m or 31m.
    """
    total_seconds = int(td.total_seconds())
    if total_seconds < 0:
        total_seconds = 0
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    if hours > 0:
        if minutes > 0:
            return f"{hours}h {minutes}m"
        return f"{hours}h"
    else:
        return f"{minutes}m"

def format_supply_log_duration(td):
    """
    Format a timedelta object to e.g. 5 mins or 41 mins.
    """
    total_seconds = int(td.total_seconds())
    if total_seconds < 0:
        total_seconds = 0
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    if hours > 0:
        if minutes > 0:
            return f"{hours}h {minutes}m"
        return f"{hours}h"
    else:
        if minutes == 1:
            return "1 min"
        return f"{minutes} mins"

def format_last_status_duration(td):
    """
    Format a timedelta object to e.g. 1min or 5mins or 1h 45m.
    """
    total_seconds = int(td.total_seconds())
    if total_seconds < 0:
        total_seconds = 0
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    if hours > 0:
        if minutes > 0:
            return f"{hours}h {minutes}m"
        return f"{hours}h"
    else:
        if minutes == 1:
            return "1min"
        return f"{minutes}mins"

def format_time_dot(dt):
    """
    Format a datetime object to h.mmam/pm in Africa/Lagos timezone (e.g. 5.13am, 12.41pm).
    """
    lagos_tz = pytz.timezone("Africa/Lagos")
    if dt.tzinfo:
        dt = dt.astimezone(lagos_tz)
    else:
        dt = lagos_tz.localize(dt)
    h_12 = dt.strftime("%I")
    minute = dt.strftime("%M")
    ampm = dt.strftime("%p").lower()
    h_12 = str(int(h_12))  # strip leading zero
    return f"{h_12}.{minute}{ampm}"

def format_duration(td):
    """
    Format a timedelta object to Xhr(s) Ymin(s) (e.g. 1hr 23mins, 4hrs 40mins).
    """
    total_seconds = int(td.total_seconds())
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    
    parts = []
    if hours > 0:
        h_str = "hr" if hours == 1 else "hrs"
        parts.append(f"{hours}{h_str}")
    if minutes > 0 or not parts:
        m_str = "min" if minutes == 1 else "mins"
        parts.append(f"{minutes}{m_str}")
    return " ".join(parts)

def send_whatsapp_power_message(number: str, text: str):
    """
    Send a text message via Evolution API to the specified contact and the power monitoring group.
    """
    base_url = EVOLUTION_API_URL.rstrip('/')
    url = f"{base_url}/message/sendText/{POWER_INSTANCE}"
    headers = {
        "apikey": EVOLUTION_API_KEY,
        "Content-Type": "application/json"
    }
    
    primary_recipient_val = None
    if number:
        try:
            with engine.connect() as conn:
                row = conn.execute(
                    text("SELECT primary_recipient FROM myapp_feeder WHERE registered_phone = :num OR msisdn = :num OR sim_serial = :num LIMIT 1"),
                    {"num": number}
                ).fetchone()
                if row and row[0]:
                    primary_recipient_val = row[0]
        except Exception as db_err:
            logger.error(f"Failed to lookup primary_recipient for {number}: {db_err}")

    recipients = []
    if primary_recipient_val:
        import re
        parts = re.split(r'[,\s;]+', primary_recipient_val)
        for part in parts:
            part = part.strip().replace("(", "").replace(")", "")
            if not part:
                continue
            if "@" in part:
                recipients.append(part)
            else:
                clean_p = part.replace("+", "").strip()
                if clean_p:
                    recipients.append(f"{clean_p}@s.whatsapp.net")

    if not recipients:
        clean_number = number.replace("+", "").strip() if number else ""
        if clean_number:
            primary_recipient = f"{clean_number}@s.whatsapp.net" if "@" not in clean_number else clean_number
            recipients.append(primary_recipient)
        
        group_id = "120363427045301423@g.us"
        if group_id not in recipients:
            recipients.append(group_id)
        
    last_response = None
    for recipient in recipients:
        logger.info(f"Sending WhatsApp power alert to {recipient}")
        payload = {
            "number": recipient,
            "text": text,
            "linkPreview": False
        }
        logger.info(f"Payload: {payload}")
        
        try:
            response = requests.post(url, json=payload, headers=headers, timeout=15)
            logger.info(f"WhatsApp power alert sent to {recipient}. Status code: {response.status_code}")
            if response.status_code in [200, 201]:
                resp_json = response.json()
                if last_response is None:
                    last_response = resp_json
        except Exception as e:
            logger.error(f"Failed to send WhatsApp power alert to {recipient}: {e}", exc_info=True)
            
    return last_response

def generate_power_report(feeder, target_date, is_today=True):
    """
    Query power status updates and reconstruct the power cycles on the target date.
    Supports real-time (today) format and End of Day (yesterday) format.
    """
    import re
    lagos_tz = pytz.timezone("Africa/Lagos")
    
    start_naive = datetime.combine(target_date, time.min)
    end_naive = datetime.combine(target_date, time.max)
    
    start_dt = lagos_tz.localize(start_naive)
    end_dt = lagos_tz.localize(end_naive)
    
    # Query updates on target date
    with engine.connect() as conn:
        updates_query = text("""
            SELECT status, server_time 
            FROM myapp_powerstatus 
            WHERE feeder_id = :feeder_id AND server_time BETWEEN :start_dt AND :end_dt 
            ORDER BY server_time
        """)
        updates = conn.execute(updates_query, {
            "feeder_id": feeder.id,
            "start_dt": start_dt,
            "end_dt": end_dt
        }).fetchall()
        
        pre_update_query = text("""
            SELECT status, server_time 
            FROM myapp_powerstatus 
            WHERE feeder_id = :feeder_id AND server_time < :start_dt 
            ORDER BY server_time DESC 
            LIMIT 1
        """)
        pre_update = conn.execute(pre_update_query, {
            "feeder_id": feeder.id,
            "start_dt": start_dt
        }).fetchone()
        
    cycles = []
    current_on = None
    
    def make_aware_lagos(dt):
        if dt is None:
            return None
        if dt.tzinfo is None:
            return lagos_tz.localize(dt)
        return dt.astimezone(lagos_tz)

    if pre_update and pre_update[0].upper() == 'ON':
        current_on = make_aware_lagos(start_dt)
        
    for status, server_time in updates:
        status_upper = status.upper()
        server_time_aware = make_aware_lagos(server_time)
        if status_upper == 'ON':
            if current_on is None:
                current_on = server_time_aware
        elif status_upper == 'OFF':
            if current_on is not None:
                cycles.append((current_on, server_time_aware))
                current_on = None
                
    if current_on is not None:
        end_of_cycle = make_aware_lagos(datetime.now(lagos_tz)) if is_today else make_aware_lagos(end_dt)
        cycles.append((current_on, end_of_cycle))
        
    # Reconstruct outages (OFF periods) for the day
    outages = []
    if cycles:
        if cycles[0][0] > make_aware_lagos(start_dt):
            outages.append((make_aware_lagos(start_dt), cycles[0][0]))
        for i in range(len(cycles) - 1):
            outages.append((cycles[i][1], cycles[i+1][0]))
        if cycles[-1][1] < make_aware_lagos(end_dt):
            outages.append((cycles[-1][1], make_aware_lagos(end_dt)))
    else:
        outages.append((make_aware_lagos(start_dt), make_aware_lagos(end_dt)))
        
    total_supply = sum((off - on for on, off in cycles), timedelta())
    
    # ------------------
    # TASK 2: Real-time update format (is_today = True)
    # ------------------
    if is_today:
        # Determine current status and when it started
        current_status = "UNKNOWN"
        current_since = None
        
        with engine.connect() as conn:
            recent_query = text("""
                SELECT status, server_time 
                FROM myapp_powerstatus 
                WHERE feeder_id = :feeder_id 
                ORDER BY server_time DESC 
                LIMIT 50
            """)
            recent_updates = conn.execute(recent_query, {"feeder_id": feeder.id}).fetchall()
            
        if recent_updates:
            current_status = recent_updates[0][0].upper()
            current_since = recent_updates[0][1]
            for status, s_time in recent_updates:
                if status.upper() == current_status:
                    current_since = s_time
                else:
                    break
        elif pre_update:
            current_status = pre_update[0].upper()
            current_since = pre_update[1]
            
        # Determine last outage duration (if currently ON) or last supply duration (if currently OFF)
        last_outage_duration = None
        last_supply_duration = None
        
        if current_status == "ON" and current_since:
            last_off_time = None
            last_off_start = None
            for status, s_time in recent_updates:
                status_upper = status.upper()
                if s_time < current_since:
                    if status_upper == "OFF":
                        if last_off_time is None:
                            last_off_time = s_time
                        last_off_start = s_time
                    elif status_upper == "ON" and last_off_time is not None:
                        break
            if last_off_start and current_since:
                last_outage_duration = current_since - last_off_start
                
        elif current_status == "OFF" and current_since:
            last_on_time = None
            last_on_start = None
            for status, s_time in recent_updates:
                status_upper = status.upper()
                if s_time < current_since:
                    if status_upper == "ON":
                        if last_on_time is None:
                            last_on_time = s_time
                        last_on_start = s_time
                    elif status_upper == "OFF" and last_on_time is not None:
                        break
            if last_on_start and current_since:
                last_supply_duration = current_since - last_on_start
                
        status_icon = "🟢" if current_status == "ON" else "🔴"
        since_str = format_time_colon(current_since) if current_since else "N/A"
        
        lines = []
        band_part = f"  | Band {feeder.band}" if getattr(feeder, 'band', None) else ""
        lines.append(f"⚡ *{feeder.name}{band_part}*")
        lines.append(f"{status_icon} *Current Status*: {current_status}  since {since_str}")
        
        if current_status == "ON":
            outage_dur_str = format_last_status_duration(last_outage_duration) if last_outage_duration else "N/A"
            lines.append(f"⏱️ Last outage was {outage_dur_str}")
        else:
            supply_dur_str = format_last_status_duration(last_supply_duration) if last_supply_duration else "N/A"
            lines.append(f"⏱️ Last supply was {supply_dur_str}")
            
        lines.append("")
        lines.append("🔹 Supply Log")
        for on_time, off_time in cycles:
            duration = off_time - on_time
            on_str = format_time_colon(on_time)
            off_str = format_time_colon(off_time)
            dur_str = format_supply_log_duration(duration)
            lines.append(f"- Power On: {on_str} → Power Off: {off_str} | ⏱️ Supply: {dur_str}")
            
        return "\n".join(lines)

    # ------------------
    # TASK 3: End of Day report format (is_today = False)
    # ------------------
    else:
        # Longest Supply
        if cycles:
            longest_supply_cycle = max(cycles, key=lambda c: c[1] - c[0])
            longest_supply_dur = longest_supply_cycle[1] - longest_supply_cycle[0]
            longest_supply_time = format_time_colon(longest_supply_cycle[0])
        else:
            longest_supply_dur = timedelta()
            longest_supply_time = "N/A"
            
        # Longest Outage
        valid_outages = [o for o in outages if o[1] - o[0] > timedelta()]
        if not valid_outages and outages:
            valid_outages = outages
            
        if valid_outages:
            longest_outage_cycle = max(valid_outages, key=lambda o: o[1] - o[0])
            longest_outage_dur = longest_outage_cycle[1] - longest_outage_cycle[0]
            longest_outage_time = format_time_colon(longest_outage_cycle[0])
        else:
            longest_outage_dur = timedelta()
            longest_outage_time = "N/A"
            
        # Average Supply
        if cycles:
            avg_supply_dur = total_supply / len(cycles)
        else:
            avg_supply_dur = timedelta()
            
        # Reliability Score
        reliability_score = int(round((total_supply.total_seconds() / 86400.0) * 100))
        
        # Hashtags
        tags = []
        feeder_clean = re.sub(r'[^a-zA-Z0-9]', '', feeder.name.split()[0])
        if feeder_clean:
            tags.append(f"#{feeder_clean}")
            
        # Get transformer name from DB
        transformer = None
        with engine.connect() as conn:
            t_query = text("SELECT transformer_name FROM myapp_feeder WHERE id = :feeder_id")
            t_row = conn.execute(t_query, {"feeder_id": feeder.id}).fetchone()
            if t_row and t_row[0]:
                transformer = t_row[0]
                
        if transformer:
            trans_clean = re.sub(r'[^a-zA-Z0-9]', '', transformer.split()[0])
            if trans_clean and trans_clean not in tags:
                tags.append(f"#{trans_clean}")
        else:
            tags.append("#Ayanngburen")
            
        tags.append("#PowerTracker")
        hashtag_str = " ".join(tags)
        
        day_name = target_date.strftime("%A")
        date_str = target_date.strftime("%d %B %Y")
        
        lines = []
        band_part = f" ( Band {feeder.band} )" if getattr(feeder, 'band', None) else ""
        lines.append("⚡ *Power Supply  Insight -Daily  Report ")
        lines.append(f" {feeder.name} {band_part}")
        lines.append("")
        lines.append(f"📅 *{day_name}*: {date_str}")
        lines.append("")
        lines.append("")
        lines.append("📊 * Supply Snippet *")
        
        total_uptime_str = format_duration_short(total_supply)
        lines.append(f"├ Total Uptime: *{total_uptime_str}*")
        lines.append(f"├ Total Outages: *{len(outages)}*")
        
        longest_supply_str = format_duration_short(longest_supply_dur)
        lines.append(f"├ Longest Supply: *{longest_supply_str}* @ {longest_supply_time}")
        
        longest_outage_str = format_duration_short(longest_outage_dur)
        lines.append(f"├ Longest Outage: *{longest_outage_str}* @ {longest_outage_time}")
        
        avg_supply_str = format_duration_short(avg_supply_dur)
        lines.append(f"└ Avg Supply per Cycle: *{avg_supply_str}*")
        lines.append("")
        
        lines.append(f"📈 *Reliability Score*: *{reliability_score}%* ")
        lines.append(f"[{total_uptime_str} / 24h]")
        lines.append("")
        
        lines.append("🔹 Supply Log")
        for on_time, off_time in cycles:
            duration = off_time - on_time
            on_str = format_time_colon(on_time)
            off_str = format_time_colon(off_time)
            dur_str = format_supply_log_duration(duration)
            lines.append(f"- Power On: {on_str} → Power Off: {off_str} | ⏱️ Supply: {dur_str}")
            
        lines.append("")
        lines.append(hashtag_str)
        
        return "\n".join(lines)

@celery_app.task(name="myapp.tasks.send_email_async")
def send_email_async(subject, text_content, html_content, to_emails, from_email=None, tenant_id=None):
    logger.info(f"Attempting to send email to {to_emails} with subject '{subject}', tenant_id: {tenant_id}")
    
    # Default values from environment variables
    smtp_username = os.getenv("GMAIL_USER") or "upwardwave.dignity@gmail.com"
    smtp_password = os.getenv("GMAIL_APP_PASSWORD") or "ybccjzqmxxlalaal"
    smtp_host = os.getenv("EMAIL_HOST", "smtp.gmail.com")
    smtp_port = int(os.getenv("EMAIL_PORT", 465))
    use_ssl = os.getenv("EMAIL_USE_SSL", "true").lower() == "true"
    
    from_header = from_email or os.getenv("DEFAULT_FROM_EMAIL") or smtp_username
    brand_name = "Vectra Laundry"
    
    if tenant_id:
        try:
            with engine.connect() as conn:
                tenant_query = text("SELECT vectra_email, password, name FROM myapp_tenant WHERE id = :id")
                tenant = conn.execute(tenant_query, {"id": tenant_id}).fetchone()
                if tenant:
                    attr_query = text("SELECT brand_name FROM myapp_tenantattribute WHERE tenant_id = :id")
                    attr = conn.execute(attr_query, {"id": tenant_id}).fetchone()
                    if attr and attr[0]:
                        brand_name = attr[0]
                    
                    from_header = f"{brand_name} <{smtp_username}>"
                    
                    if tenant[0] and tenant[1]:  # custom email credentials
                        logger.info(f"🔧 Using CUSTOM connection parameters for tenant {tenant_id}")
                        smtp_username = tenant[0]
                        smtp_password = tenant[1]
                        from_header = f"{brand_name} <{smtp_username}>"
                else:
                    logger.warning(f"Tenant {tenant_id} not found.")
        except Exception as e:
            logger.error(f"Error retrieving Tenant info from database: {e}", exc_info=True)

    # Send Email via smtplib
    try:
        logger.info(f"📧 Sending as: {from_header} (Auth User: {smtp_username})")
        
        msg = MIMEMultipart()
        msg['Subject'] = subject
        msg['From'] = from_header
        
        if isinstance(to_emails, list):
            msg['To'] = ", ".join(to_emails)
            recipients = to_emails
        else:
            msg['To'] = to_emails
            recipients = [to_emails]
            
        msg.attach(MIMEText(text_content, 'plain'))
        if html_content:
            msg.attach(MIMEText(html_content, 'html'))
            
        if use_ssl:
            with smtplib.SMTP_SSL(smtp_host, smtp_port, timeout=15) as server:
                server.login(smtp_username, smtp_password)
                server.sendmail(smtp_username, recipients, msg.as_string())
        else:
            with smtplib.SMTP(smtp_host, smtp_port, timeout=15) as server:
                server.starttls()
                server.login(smtp_username, smtp_password)
                server.sendmail(smtp_username, recipients, msg.as_string())
                
        logger.info(f"✅ Email sent successfully from: {from_header}")
        return True
    except Exception as e:
        logger.error(f"❌ Failed to send email: {e}", exc_info=True)
        return False

@celery_app.task(name="myapp.tasks.add")
def add(x, y):
    return x + y

@celery_app.task(name="myapp.tasks.send_test_email")
def send_test_email():
    """
    Test task to validate send_email_async via Celery.
    """
    subject = "Celery Validation Test Using Send Test Email Function"
    text_content = "This is a test email sent via Celery using send_email_async."
    html_content = "<p>This is a <b>test email</b> sent via Celery using send_email_async.</p>"
    to_emails = ["ayodelefestusng@gmail.com"]
    return send_email_async(subject, text_content, html_content, to_emails)

@celery_app.task(name="myapp.tasks.send_test_email1")
def send_test_email1():
    return send_email_async(
        subject="Test Email",
        text_content="This is a test email with fallback.",
        html_content=None,
        to_emails=["buyriteautosng@gmail.com"]
    )

@celery_app.task(name="myapp.tasks.send_power_email", bind=True, default_retry_delay=15, max_retries=10, autoretry_for=(Exception,))
def send_power_email(self, feeder_name, status, device_time, server_time, contact_phone=None, transformer_name="UNKNOWN_TRANSFORMER", peak_a0=0, msisdn="UNKNOWN", sim_serial="UNKNOWN"):
    logger.info(f"Processing real-time power update for Feeder {feeder_name} with status {status}")
    
    # 1. Database Persistence
    feeder = None
    try:
        from worker.main import save_power_status_update, PowerStatus
        
        # Parse server_time string to datetime object (timezone-aware)
        lagos_tz = pytz.timezone("Africa/Lagos")
        try:
            server_time_dt = datetime.strptime(server_time, "%Y-%m-%d %H:%M:%S.%f")
        except ValueError:
            try:
                server_time_dt = datetime.strptime(server_time, "%Y-%m-%d %H:%M:%S")
            except ValueError:
                server_time_dt = datetime.now()
        
        if server_time_dt.tzinfo is None:
            server_time_dt = lagos_tz.localize(server_time_dt)

        # Construct PowerStatus object
        data = PowerStatus(
            status=status,
            timestamp=int(device_time),
            peak_a0=int(peak_a0),
            feeder_name=feeder_name,
            transformer_code=transformer_name,
            sim_serial=sim_serial if sim_serial != "UNKNOWN" else (contact_phone or "UNKNOWN"),
            contact_phone=contact_phone,
            msisdn=msisdn
        )
        
        # Save power status and get feeder_id
        feeder_id = save_power_status_update(data, server_time_dt)
        
        # Retrieve the updated Feeder details for reporting
        with engine.connect() as conn:
            feeder_query = text("SELECT id, name, registered_phone, band FROM myapp_feeder WHERE id = :id")
            row = conn.execute(feeder_query, {"id": feeder_id}).fetchone()
            if row:
                feeder = FeederObj(row[0], row[1], row[2], row[3])
            else:
                feeder = FeederObj(feeder_id, feeder_name, contact_phone)
                
    except Exception as db_err:
        logger.error(f"Database persistence failed, task will be retried: {db_err}", exc_info=True)
        # Allow Celery to retry by propagating the exception
        raise db_err

    # 2. Reconstruct today's log cycles report
    if feeder is None:
        feeder = FeederObj(0, feeder_name, contact_phone)
        
    try:
        lagos_tz = pytz.timezone("Africa/Lagos")
        today_date = datetime.now(lagos_tz).date()
        body = generate_power_report(feeder, today_date, is_today=True)
    except Exception as e:
        logger.error(f"Error generating power report: {e}", exc_info=True)
        body = f"{feeder_name} status is {status}\nServer time: {server_time}"

    # 3. Send Email Alert
    try:
        gmail_user = os.getenv("GMAIL_USER") or "upwardwave.dignity@gmail.com"
        gmail_password = os.getenv("GMAIL_APP_PASSWORD") or "ybccjzqmxxlalaal"
        to_email = os.getenv("ALERT_RECIPIENT") or "ayodelefestusng@gmail.com"

        subject = f"ALERT: Grid Power is {status.upper()} - {feeder_name}"

        msg = MIMEMultipart()
        msg['From'] = gmail_user
        msg['To'] = to_email
        msg['Subject'] = subject
        msg.attach(MIMEText(body, 'plain'))

        with smtplib.SMTP_SSL('smtp.gmail.com', 465, timeout=15) as server:
            server.login(gmail_user, gmail_password)
            server.sendmail(gmail_user, to_email, msg.as_string())
        logger.info(f"Power alert email sent successfully to {to_email} for Feeder {feeder_name}")
    except Exception as e:
        logger.error(f"Failed to send email alert for Feeder {feeder_name}: {e}", exc_info=True)

    # 4. Send WhatsApp Alert
    try:
        phone_to_use = contact_phone or feeder.contact_phone
        if phone_to_use:
            send_whatsapp_power_message(phone_to_use, body)
        else:
            logger.warning(f"No contact phone available to send WhatsApp message for Feeder {feeder_name}")
    # except Exception as e:
    #     logger.error(f"Failed to send WhatsApp power alert (Evolution API dispatch failed): {e}", exc_info=True)
    except Exception as exc:
        logger.error(f"Task failed, will retry: {exc}", exc_info=True)
        raise self.retry(exc=exc)
@celery_app.task(name="myapp.tasks.send_daily_power_updates")
def send_daily_power_updates():
    logger.info("Executing periodic daily power summary updates task")
    
    lagos_tz = pytz.timezone("Africa/Lagos")
    yesterday = (datetime.now(lagos_tz) - timedelta(days=1)).date()
    
    feeders = []
    try:
        with engine.connect() as conn:
            feeders_query = text("SELECT id, name, registered_phone, band FROM myapp_feeder")
            rows = conn.execute(feeders_query).fetchall()
            for r in rows:
                feeders.append(FeederObj(r[0], r[1], r[2], r[3]))
    except Exception as e:
        logger.error(f"Error fetching Feeders: {e}", exc_info=True)
        
    if not feeders:
        logger.info("No feeders found in database for daily updates.")
        return
        
    gmail_user = os.getenv("GMAIL_USER") or "upwardwave.dignity@gmail.com"
    gmail_password = os.getenv("GMAIL_APP_PASSWORD") or "ybccjzqmxxlalaal"
    to_email = os.getenv("ALERT_RECIPIENT") or "ayodelefestusng@gmail.com"
    
    for feeder in feeders:
        try:
            body = generate_power_report(feeder, yesterday, is_today=False)
            subject = f"DAILY POWER SUMMARY: {feeder.name} - {yesterday.strftime('%d/%m/%Y')}"
            
            # Send Email
            msg = MIMEMultipart()
            msg['From'] = gmail_user
            msg['To'] = to_email
            msg['Subject'] = subject
            msg.attach(MIMEText(body, 'plain'))
            
            try:
                with smtplib.SMTP_SSL('smtp.gmail.com', 465, timeout=15) as server:
                    server.login(gmail_user, gmail_password)
                    server.sendmail(gmail_user, to_email, msg.as_string())
                logger.info(f"Daily summary email sent for Feeder {feeder.name}")
            except Exception as e:
                logger.error(f"Failed to send daily summary email for Feeder {feeder.name}: {e}", exc_info=True)
                
            # Send WhatsApp
            if feeder.contact_phone:
                send_whatsapp_power_message(feeder.contact_phone, body)
            else:
                logger.info(f"No contact phone available to send daily summary WhatsApp for Feeder {feeder.name}")
                
        except Exception as e:
            logger.error(f"Error generating daily summary report for Feeder {feeder.name}: {e}", exc_info=True)

@celery_app.task(name="myapp.tasks.send_security_alert_email")
def send_security_alert_email(feeder_name, transformer_name, contact_phone, msisdn, server_time):
    logger.warning(
        f"🚨 SECURITY ALERT: Hardware SIM mismatch detected for Feeder: {feeder_name} "
        f"({transformer_name}). Expected contact: {contact_phone}, received SIM: {msisdn} "
        f"at {server_time}."
    )
    
    gmail_user = os.getenv("GMAIL_USER") or "upwardwave.dignity@gmail.com"
    gmail_password = os.getenv("GMAIL_APP_PASSWORD") or "ybccjzqmxxlalaal"
    to_email = os.getenv("ALERT_RECIPIENT") or "ayodelefestusng@gmail.com"

    subject = f"🚨 SECURITY ALERT: SIM Mismatch for {feeder_name}"
    
    body = (
        f"CRITICAL SECURITY ALERT\n"
        f"=======================\n\n"
        f"A hardware SIM card identity mismatch has been detected on the power tracker network.\n\n"
        f"Feeder Name: {feeder_name}\n"
        f"Transformer: {transformer_name}\n"
        f"Designated Contact: {contact_phone}\n"
        f"Active SIM MSISDN: {msisdn}\n"
        f"Detection Server Time: {server_time}\n\n"
        f"Please verify the hardware node immediately to prevent unauthorized access or tampering."
    )

    msg = MIMEMultipart()
    msg['From'] = gmail_user
    msg['To'] = to_email
    msg['Subject'] = subject
    msg.attach(MIMEText(body, 'plain'))

    try:
        with smtplib.SMTP_SSL('smtp.gmail.com', 465, timeout=15) as server:
            server.login(gmail_user, gmail_password)
            server.sendmail(gmail_user, to_email, msg.as_string())
        logger.info(f"Security alert email sent successfully to {to_email} for Feeder {feeder_name}")
    except Exception as e:
        logger.error(f"Failed to send security alert email: {e}", exc_info=True)

    phone_to_use = contact_phone
    if phone_to_use:
        send_whatsapp_power_message(phone_to_use, body)
    else:
        logger.warning("No contact phone available to send security alert WhatsApp.")
