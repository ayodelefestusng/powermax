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
    def __init__(self, id, name, contact_phone):
        self.id = id
        self.name = name
        self.contact_phone = contact_phone

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
    
    clean_number = number.replace("+", "").strip()
    primary_recipient = f"{clean_number}@s.whatsapp.net" if "@" not in clean_number else clean_number
    
    recipients = [primary_recipient]
    group_id = "120363406600149982@g.us"
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
                if recipient == primary_recipient:
                    last_response = resp_json
                elif last_response is None:
                    last_response = resp_json
        except Exception as e:
            logger.error(f"Failed to send WhatsApp power alert to {recipient}: {e}", exc_info=True)
            
    return last_response

def generate_power_report(feeder, target_date, is_today=True):
    """
    Query power status updates and reconstruct the power cycles on the target date.
    """
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
        
    # Format cycles and compute supply duration
    lines = []
    total_supply = timedelta()
    
    date_str = target_date.strftime("%d/%m/%Y")
    day_label = "today" if is_today else "yesterday"
    lines.append(f"{feeder.name} as @ {day_label} {date_str}")
    
    for on_time, off_time in cycles:
        duration = off_time - on_time
        total_supply += duration
        
        on_str = format_time_dot(on_time)
        off_str = format_time_dot(off_time)
        dur_str = format_duration(duration)
        
        lines.append(f"Power on: {on_str}")
        lines.append(f"Power off: {off_str}")
        lines.append(f"Supply {dur_str}")
        
    lines.append("")
    total_supply_str = format_duration(total_supply)
    lines.append(f"Total Supply {total_supply_str}")
    
    if not is_today:
        total_outage = timedelta(hours=24) - total_supply
        if total_outage < timedelta():
            total_outage = timedelta()
        total_outage_str = format_duration(total_outage)
        lines.append(f"Total Outage  {total_outage_str}")
        
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

@celery_app.task(name="myapp.tasks.send_power_email")
def send_power_email(feeder_name, status, device_time, server_time, contact_phone=None):
    logger.info(f"Processing real-time power update for Feeder {feeder_name} with status {status}")
    
    # 1. Fetch/Create Feeder
    feeder = None
    try:
        with engine.begin() as conn:
            feeder_query = text("SELECT id, name, contact_phone FROM myapp_feeder WHERE name = :name")
            row = conn.execute(feeder_query, {"name": feeder_name}).fetchone()
            if not row:
                insert_query = text("""
                    INSERT INTO myapp_feeder (name, contact_phone, created_at)
                    VALUES (:name, :contact_phone, :created_at)
                    RETURNING id, name, contact_phone
                """)
                row = conn.execute(insert_query, {
                    "name": feeder_name,
                    "contact_phone": contact_phone,
                    "created_at": datetime.now()
                }).fetchone()
            else:
                if contact_phone and row[2] != contact_phone:
                    update_query = text("UPDATE myapp_feeder SET contact_phone = :phone WHERE id = :id")
                    conn.execute(update_query, {"phone": contact_phone, "id": row[0]})
                    row = (row[0], row[1], contact_phone)
            
            feeder = FeederObj(row[0], row[1], row[2])
    except Exception as e:
        logger.error(f"Error retrieving or creating Feeder: {e}", exc_info=True)
        feeder = FeederObj(0, feeder_name, contact_phone)

    # 2. Reconstruct today's log cycles report
    try:
        lagos_tz = pytz.timezone("Africa/Lagos")
        today_date = datetime.now(lagos_tz).date()
        body = generate_power_report(feeder, today_date, is_today=True)
    except Exception as e:
        logger.error(f"Error generating power report: {e}", exc_info=True)
        body = f"{feeder_name} status is {status}\nServer time: {server_time}"

    # 3. Send Email Alert
    gmail_user = os.getenv("GMAIL_USER") or "upwardwave.dignity@gmail.com"
    gmail_password = os.getenv("GMAIL_APP_PASSWORD") or "ybccjzqmxxlalaal"
    to_email = os.getenv("ALERT_RECIPIENT") or "ayodelefestusng@gmail.com"

    subject = f"ALERT: Grid Power is {status.upper()} - {feeder_name}"

    msg = MIMEMultipart()
    msg['From'] = gmail_user
    msg['To'] = to_email
    msg['Subject'] = subject
    msg.attach(MIMEText(body, 'plain'))

    try:
        with smtplib.SMTP_SSL('smtp.gmail.com', 465, timeout=15) as server:
            server.login(gmail_user, gmail_password)
            server.sendmail(gmail_user, to_email, msg.as_string())
        logger.info(f"Power alert email sent successfully to {to_email} for Feeder {feeder_name}")
    except Exception as e:
        logger.error(f"Failed to send email alert for Feeder {feeder_name}: {e}", exc_info=True)

    # 4. Send WhatsApp Alert
    phone_to_use = contact_phone or feeder.contact_phone
    if phone_to_use:
        send_whatsapp_power_message(phone_to_use, body)
    else:
        logger.warning(f"No contact phone available to send WhatsApp message for Feeder {feeder_name}")

@celery_app.task(name="myapp.tasks.send_daily_power_updates")
def send_daily_power_updates():
    logger.info("Executing periodic daily power summary updates task")
    
    lagos_tz = pytz.timezone("Africa/Lagos")
    yesterday = (datetime.now(lagos_tz) - timedelta(days=1)).date()
    
    feeders = []
    try:
        with engine.connect() as conn:
            feeders_query = text("SELECT id, name, contact_phone FROM myapp_feeder")
            rows = conn.execute(feeders_query).fetchall()
            for r in rows:
                feeders.append(FeederObj(r[0], r[1], r[2]))
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
