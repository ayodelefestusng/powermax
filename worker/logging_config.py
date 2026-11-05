"""
Shared logging configuration module for power_max (FastAPI Gateway)
and power_max_celery (Celery Worker & Beat).

Ensures consistent logging format, dual output (Docker stdout + rotating file log),
and automatic volume directory handling across production and local environments.
"""

import logging
import os
import sys
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Optional, Union

# Standard log format across all services
LOG_FORMAT = "%(asctime)s %(levelname)s %(name)s %(message)s"

# Default configuration values
DEFAULT_LOG_DIR = Path("/app/logs")
DEFAULT_MAX_BYTES = 10 * 1024 * 1024  # 10MB
DEFAULT_BACKUP_COUNT = 10
DEFAULT_LOG_LEVEL = logging.INFO


def get_log_dir(custom_dir: Optional[Union[str, Path]] = None) -> Path:
    """
    Resolve and create the log directory.
    Priority:
      1. Explicit custom_dir parameter
      2. LOG_DIR environment variable
      3. /app/logs (production mounted volume in Docker)
      4. Local fallback (<package_root>/logs) for local development (e.g. Windows)
    """
    if custom_dir:
        path = Path(custom_dir)
        path.mkdir(parents=True, exist_ok=True)
        return path

    env_dir = os.getenv("LOG_DIR")
    if env_dir:
        path = Path(env_dir)
        path.mkdir(parents=True, exist_ok=True)
        return path

    # Attempt production mounted volume location
    prod_path = DEFAULT_LOG_DIR
    try:
        prod_path.mkdir(parents=True, exist_ok=True)
        return prod_path
    except (OSError, PermissionError):
        # Graceful fallback for local development environments
        local_path = Path(__file__).resolve().parent / "logs"
        local_path.mkdir(parents=True, exist_ok=True)
        return local_path


def configure_logging(
    log_filename: str = "app.log",
    logger_name: Optional[str] = None,
    log_dir: Optional[Union[str, Path]] = None,
    level: int = DEFAULT_LOG_LEVEL,
    log_format: str = LOG_FORMAT,
    max_bytes: int = DEFAULT_MAX_BYTES,
    backup_count: int = DEFAULT_BACKUP_COUNT,
    force: bool = False,
) -> logging.Logger:
    """
    Configure root logging with Docker stdout and persistent rotating file log.
    Prevents duplicate handlers on reload or multiple calls unless force=True.
    """
    resolved_dir = get_log_dir(log_dir)
    log_file = resolved_dir / log_filename
    target_file_str = str(log_file.resolve())

    root_logger = logging.getLogger()
    root_logger.setLevel(level)

    # If force is requested, remove existing handlers
    if force:
        root_logger.handlers.clear()

    # Prevent duplicate handlers on reload/multiple imports
    has_console = any(
        isinstance(h, logging.StreamHandler)
        and not isinstance(h, RotatingFileHandler)
        and getattr(h, "stream", None) == sys.stdout
        for h in root_logger.handlers
    )

    has_file = any(
        isinstance(h, RotatingFileHandler)
        and getattr(h, "baseFilename", "") == target_file_str
        for h in root_logger.handlers
    )

    formatter = logging.Formatter(log_format)

    # 1. Docker stdout stream handler
    if not has_console and (not root_logger.handlers or force):
        console_handler = logging.StreamHandler(sys.stdout)
        console_handler.setFormatter(formatter)
        root_logger.addHandler(console_handler)

    # 2. Persistent rotating file handler
    if not has_file and (not root_logger.handlers or not any(isinstance(h, RotatingFileHandler) for h in root_logger.handlers) or force):
        file_handler = RotatingFileHandler(
            log_file,
            maxBytes=max_bytes,
            backupCount=backup_count,
            encoding="utf-8",
        )
        file_handler.setFormatter(formatter)
        root_logger.addHandler(file_handler)

    return logging.getLogger(logger_name)


def setup_power_max_logging(
    logger_name: str = "PowerMonitor",
    log_filename: str = "app.log",
    **kwargs,
) -> logging.Logger:
    """
    Logging configuration for the power_max FastAPI Gateway application.
    Logs to stdout and /app/logs/app.log (or local fallback).
    """
    return configure_logging(
        log_filename=log_filename,
        logger_name=logger_name,
        **kwargs,
    )


# Flag to track whether Celery signals have been attached
_celery_signals_attached = False


def setup_celery_logging(
    logger_name: Optional[str] = None,
    log_filename: str = "celery.log",
    **kwargs,
) -> logging.Logger:
    """
    Logging configuration for power_max_celery (Celery Worker, Beat, and Background Tasks).
    Logs to stdout and /app/logs/celery.log (or local fallback).
    Also connects to Celery signals (after_setup_logger, after_setup_task_logger)
    to guarantee that Celery worker internals and tasks route to celery.log.
    """
    global _celery_signals_attached

    logger = configure_logging(
        log_filename=log_filename,
        logger_name=logger_name,
        **kwargs,
    )

    # Connect to Celery signals if Celery is installed
    if not _celery_signals_attached:
        try:
            from celery.signals import after_setup_logger, after_setup_task_logger

            resolved_dir = get_log_dir(kwargs.get("log_dir"))
            log_file = resolved_dir / log_filename
            target_file_str = str(log_file.resolve())
            log_format = kwargs.get("log_format", LOG_FORMAT)
            max_bytes = kwargs.get("max_bytes", DEFAULT_MAX_BYTES)
            backup_count = kwargs.get("backup_count", DEFAULT_BACKUP_COUNT)

            def _attach_celery_file_handler(logger=None, **_):
                if logger is None:
                    return
                # Avoid attaching duplicate file handler to Celery loggers
                if not any(
                    isinstance(h, RotatingFileHandler)
                    and getattr(h, "baseFilename", "") == target_file_str
                    for h in logger.handlers
                ):
                    fh = RotatingFileHandler(
                        log_file,
                        maxBytes=max_bytes,
                        backupCount=backup_count,
                        encoding="utf-8",
                    )
                    fh.setFormatter(logging.Formatter(log_format))
                    logger.addHandler(fh)

            after_setup_logger.connect(_attach_celery_file_handler, weak=False)
            after_setup_task_logger.connect(_attach_celery_file_handler, weak=False)
            _celery_signals_attached = True
        except ImportError:
            pass

    return logger


def get_logger(name: Optional[str] = None) -> logging.Logger:
    """Convenience accessor to retrieve a named logger."""
    return logging.getLogger(name)

