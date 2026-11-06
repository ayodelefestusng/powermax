"""
Database migration script:
1. Creates table myapp_transformer (FK feeder_id -> myapp_feeder.id)
2. Copies existing transformer_name and transformer_code from myapp_feeder into myapp_transformer
3. Drops transformer_name and transformer_code columns from myapp_feeder
4. Records 0048_feeder_transformer_migration in django_migrations
"""

import sys
import os
from pathlib import Path
from datetime import datetime, timezone

# Ensure worker package is in sys.path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from sqlalchemy import text
from worker.db import engine


def run_migration():
    print(f"Connecting to database via worker.db engine...")
    with engine.begin() as conn:
        # Detect database dialect
        dialect_name = engine.dialect.name
        print(f"Database dialect: {dialect_name}")

        # 1. Create myapp_transformer table if not exists
        print("Ensuring myapp_transformer table exists...")
        if dialect_name == "postgresql":
            conn.execute(text("""
                CREATE TABLE IF NOT EXISTS myapp_transformer (
                    id BIGSERIAL PRIMARY KEY,
                    feeder_id BIGINT NOT NULL REFERENCES myapp_feeder(id) ON DELETE CASCADE,
                    name VARCHAR(255) NOT NULL,
                    transformer_code VARCHAR(100) NOT NULL,
                    admin_main VARCHAR(254),
                    admin_secondary VARCHAR(254),
                    is_active BOOLEAN NOT NULL DEFAULT TRUE,
                    CONSTRAINT myapp_transformer_feeder_id_transformer_code_uniq UNIQUE (feeder_id, transformer_code)
                );
                CREATE INDEX IF NOT EXISTS myapp_transformer_feeder_id_idx ON myapp_transformer(feeder_id);
            """))
        else:
            conn.execute(text("""
                CREATE TABLE IF NOT EXISTS myapp_transformer (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    feeder_id INTEGER NOT NULL REFERENCES myapp_feeder(id) ON DELETE CASCADE,
                    name VARCHAR(255) NOT NULL,
                    transformer_code VARCHAR(100) NOT NULL,
                    admin_main VARCHAR(254),
                    admin_secondary VARCHAR(254),
                    is_active BOOLEAN NOT NULL DEFAULT 1,
                    UNIQUE (feeder_id, transformer_code)
                );
                CREATE INDEX IF NOT EXISTS myapp_transformer_feeder_id_idx ON myapp_transformer(feeder_id);
            """))
        print("✓ myapp_transformer table ready.")

        # 2. Check if columns exist in myapp_feeder
        if dialect_name == "postgresql":
            cols_query = text("""
                SELECT column_name 
                FROM information_schema.columns 
                WHERE table_name = 'myapp_feeder' AND column_name IN ('transformer_name', 'transformer_code')
            """)
            existing_cols = [r[0] for r in conn.execute(cols_query).fetchall()]
        else:
            pragma_res = conn.execute(text("PRAGMA table_info(myapp_feeder)")).fetchall()
            existing_cols = [r[1] for r in pragma_res if r[1] in ('transformer_name', 'transformer_code')]

        print(f"Columns on myapp_feeder: {existing_cols}")

        if "transformer_name" in existing_cols or "transformer_code" in existing_cols:
            print("Migrating records from myapp_feeder to myapp_transformer...")
            if dialect_name == "postgresql":
                name_col = "transformer_name" if "transformer_name" in existing_cols else "name"
                code_col = "transformer_code" if "transformer_code" in existing_cols else "'UNKNOWN_TRANSFORMER'"
                migrate_sql = f"""
                    INSERT INTO myapp_transformer (feeder_id, name, transformer_code, is_active)
                    SELECT 
                        id, 
                        COALESCE(NULLIF(TRIM({name_col}), ''), TRIM(name), 'UNKNOWN_TRANSFORMER'), 
                        COALESCE(NULLIF(TRIM({code_col}), ''), 'UNKNOWN_TRANSFORMER'), 
                        TRUE
                    FROM myapp_feeder
                    WHERE ({name_col} IS NOT NULL AND TRIM({name_col}) != '')
                       OR ({code_col} IS NOT NULL AND TRIM({code_col}) != '')
                    ON CONFLICT (feeder_id, transformer_code) DO NOTHING;
                """
                conn.execute(text(migrate_sql))
            else:
                rows = conn.execute(text(f"SELECT id, name, transformer_name, transformer_code FROM myapp_feeder")).fetchall()
                for r in rows:
                    feeder_id, f_name, tf_name, tf_code = r[0], r[1], r[2], r[3]
                    resolved_name = (tf_name or f_name or "UNKNOWN_TRANSFORMER").strip()
                    resolved_code = (tf_code or "UNKNOWN_TRANSFORMER").strip()
                    conn.execute(
                        text("""
                            INSERT OR IGNORE INTO myapp_transformer (feeder_id, name, transformer_code, is_active)
                            VALUES (:feeder_id, :name, :code, 1)
                        """),
                        {"feeder_id": feeder_id, "name": resolved_name, "code": resolved_code}
                    )
            print("✓ Existing feeder transformer data migrated to myapp_transformer.")

            # 3. Drop columns from myapp_feeder
            if dialect_name == "postgresql":
                print("Dropping transformer_name and transformer_code columns from myapp_feeder...")
                conn.execute(text("ALTER TABLE myapp_feeder DROP COLUMN IF EXISTS transformer_name;"))
                conn.execute(text("ALTER TABLE myapp_feeder DROP COLUMN IF EXISTS transformer_code;"))
                print("✓ Columns dropped from myapp_feeder.")
            else:
                print("SQLite dialect detected; column drop skipped (handled via Django schema migration).")
        else:
            print("Columns already removed from myapp_feeder.")

        # 4. Check & record in django_migrations table
        if dialect_name == "postgresql":
            dm_check = conn.execute(text("""
                SELECT table_name FROM information_schema.tables WHERE table_name = 'django_migrations'
            """)).fetchone()
        else:
            dm_check = conn.execute(text("SELECT name FROM sqlite_master WHERE type='table' AND name='django_migrations'")).fetchone()

        if dm_check:
            now_utc = datetime.now(timezone.utc)
            conn.execute(
                text("""
                    INSERT INTO django_migrations (app, name, applied)
                    SELECT 'myapp', '0048_feeder_transformer_migration', :now
                    WHERE NOT EXISTS (
                        SELECT 1 FROM django_migrations WHERE app = 'myapp' AND name = '0048_feeder_transformer_migration'
                    )
                """),
                {"now": now_utc}
            )
            print("✓ Recorded 0048_feeder_transformer_migration in django_migrations.")

        # Print current count of transformers
        count = conn.execute(text("SELECT COUNT(*) FROM myapp_transformer")).scalar()
        print(f"Total transformers in myapp_transformer: {count}")

        sample = conn.execute(text("SELECT id, feeder_id, name, transformer_code FROM myapp_transformer LIMIT 10")).fetchall()
        print("Sample transformer records:")
        for s in sample:
            print(f"  ID={s[0]}, feeder_id={s[1]}, name={s[2]}, code={s[3]}")

    print("\n✅ Migration completed successfully!")


if __name__ == "__main__":
    run_migration()

