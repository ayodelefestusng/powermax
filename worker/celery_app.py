import os
import re
import urllib.parse
import logging
from celery import Celery
from celery.schedules import crontab
from dotenv import load_dotenv

# Load env variables
load_dotenv()

logger = logging.getLogger(__name__)

def get_broker_and_backend_urls():
    """
    Dynamically detect and configure broker and result backend URLs based on WORKER_URL
    or discrete RABBITMQ_* / REDIS_URL environment variables.
    Allows seamless switching between Redis and RabbitMQ.
    """
    broker_type_env = os.getenv("BROKER_TYPE", "").strip().lower()
    raw_worker_url = os.getenv("WORKER_URL", "").strip()
    redis_fallback = os.getenv("REDIS_URL", "").strip()
    result_backend_env = os.getenv("CELERY_RESULT_BACKEND", "").strip()

    # Helper to construct RabbitMQ AMQP broker URL
    def build_rabbitmq_url():
        rmq_user = os.getenv("RABBITMQ_DEFAULT_USER", "agobadaniel")
        rmq_pass = os.getenv("RABBITMQ_DEFAULT_PASS", "@Ajibandele13&")
        rmq_host = os.getenv("RABBITMQ_HOST", "vectra-rabbitmq")
        rmq_port = os.getenv("RABBITMQ_PORT", "5672")
        rmq_vhost = os.getenv("RABBITMQ_DEFAULT_VHOST", "/")
        quoted_user = urllib.parse.quote(rmq_user, safe='')
        quoted_pass = urllib.parse.quote(rmq_pass, safe='')
        vhost_part = "%2F" if rmq_vhost == "/" else urllib.parse.quote(rmq_vhost.lstrip("/"), safe='')
        return f"amqp://{quoted_user}:{quoted_pass}@{rmq_host}:{rmq_port}/{vhost_part}"

    # Determine whether RabbitMQ is active
    use_rabbitmq = (
        broker_type_env == "rabbitmq"
        or raw_worker_url.startswith("amqp://")
        or (not raw_worker_url and bool(os.getenv("RABBITMQ_HOST") or os.getenv("RABBITMQ_DEFAULT_USER")))
    )

    if use_rabbitmq:
        if raw_worker_url.startswith("amqp://"):
            broker_url = raw_worker_url
        else:
            broker_url = build_rabbitmq_url()

        # Result backend: prefer CELERY_RESULT_BACKEND, then fallback to rpc://
        if result_backend_env:
            backend_url = result_backend_env
        else:
            backend_url = "rpc://"
        broker_type = "RabbitMQ"

    elif raw_worker_url.startswith("redis://"):
        base_redis_url = re.sub(r'/[0-9]*$', '', raw_worker_url)
        broker_url = f"{base_redis_url}/0"
        backend_url = result_backend_env or f"{base_redis_url}/0"
        broker_type = "Redis"

    elif redis_fallback and redis_fallback.startswith("redis://"):
        base_redis_url = re.sub(r'/[0-9]*$', '', redis_fallback)
        broker_url = f"{base_redis_url}/0"
        backend_url = result_backend_env or f"{base_redis_url}/0"
        broker_type = "Redis"

    else:
        broker_url = raw_worker_url or "rpc://"
        backend_url = result_backend_env or broker_url
        broker_type = "Custom"

    masked_broker = re.sub(r':([^@:]+)@', ':***@', broker_url)
    logger.info(f"Celery broker initialized ({broker_type}): {masked_broker}")
    print(f"[Celery] Configured broker ({broker_type}): {masked_broker}")

    return broker_url, backend_url, broker_type

BROKER_URL, BACKEND_URL, BROKER_TYPE = get_broker_and_backend_urls()

# Initialize Celery app
celery_app = Celery(
    "worker",
    broker=BROKER_URL,
    backend=BACKEND_URL,
    include=["worker.tasks"]
)

# Configure Celery
celery_app.conf.update(
    timezone="Africa/Lagos",
    enable_utc=False,
    task_serializer="json",
    accept_content=["json"],
    result_serializer="json",
    result_expires=3600,
    imports=["worker.tasks"],

    # Connection retry & reliability settings
    task_acks_late=True,
    task_always_eager=os.getenv("CELERY_ALWAYS_EAGER", "False").lower() == "true",
    task_worker_lost=30,
    broker_connection_retry=True,
    broker_connection_retry_on_startup=True,
    broker_connection_max_retries=None,
    broker_connection_timeout=30,
    worker_prefetch_multiplier=1,
    worker_max_tasks_per_child=1000,
    broker_pool_limit=None,
)

# Celery Beat schedule for periodic tasks
celery_app.conf.beat_schedule = {
    "daily-power-summary": {
        "task": "myapp.tasks.send_daily_power_updates",
        "schedule": crontab(minute="1", hour="0"),  # Runs daily at 00:01 Lagos time
    }
}
