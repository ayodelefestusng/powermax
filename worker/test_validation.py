import sys
from pathlib import Path
from datetime import datetime, timezone, timedelta

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from sqlalchemy import text
from worker.db import engine
from worker.main import save_power_status_update, PowerStatus
from worker.tasks import FeederObj, generate_power_report

lagos_tz = timezone(timedelta(hours=1))
now_lagos = datetime.now(lagos_tz)

print("--- Testing save_power_status_update with new schema ---")
data = PowerStatus(
    status="ON",
    timestamp=int(now_lagos.timestamp()),
    peak_a0=120,
    feeder_name="98 Ayangburen", # Existing feeder
    transformer_code="Cable Oremolu DT",
    sim_serial="TEST_SIM_123",
    contact_phone="2348021299221",
    msisdn="2348021299221",
)

feeder_id, status_changed, event_id = save_power_status_update(data, now_lagos)
print(f"save_power_status_update executed: feeder_id={feeder_id}, status_changed={status_changed}, event_id={event_id}")

with engine.connect() as conn:
    # Verify myapp_feeder has no transformer columns
    cols = [r[0] for r in conn.execute(text("SELECT column_name FROM information_schema.columns WHERE table_name = 'myapp_feeder'")).fetchall()]
    assert "transformer_name" not in cols, "transformer_name should not be in myapp_feeder"
    assert "transformer_code" not in cols, "transformer_code should not be in myapp_feeder"
    print("✓ Confirmed: transformer_name and transformer_code are NOT in myapp_feeder.")

    # Verify myapp_transformer has the record
    tf_row = conn.execute(
        text("SELECT id, feeder_id, name, transformer_code FROM myapp_transformer WHERE feeder_id = :fid"),
        {"fid": feeder_id}
    ).fetchall()
    print(f"✓ Confirmed: myapp_transformer records for feeder {feeder_id}: {tf_row}")
    assert len(tf_row) > 0, "Transformer record should exist in myapp_transformer"

print("\n--- Testing generate_power_report transformer query ---")
feeder_obj = FeederObj(feeder_id, "98 Ayangburen", "2348021299221", "A")
today_date = now_lagos.date()
report = generate_power_report(feeder_obj, today_date, is_today=True)
print("✓ generate_power_report executed successfully:")
print("  First line of report:", report.splitlines()[0] if report else "None")

print("\n✅ All validations passed with 100% success!")

