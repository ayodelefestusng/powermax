from celery import Celery
from celery.schedules import crontab
import os
from dotenv import load_dotenv

# Load env variables
load_dotenv()

REDIS_URL = os.getenv("REDIS_URL", "redis://localhost:6379/0")

# Initialize Celery app
celery_app = Celery(
    "worker",
    broker=REDIS_URL,
    backend=REDIS_URL,
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
    imports=["worker.tasks"]
)

# Celery Beat schedule for periodic tasks
celery_app.conf.beat_schedule = {
    "daily-power-summary": {
        "task": "myapp.tasks.send_daily_power_updates",
        "schedule": crontab(minute="1", hour="0"),  # Runs daily at 00:01 Lagos time
    }
}
