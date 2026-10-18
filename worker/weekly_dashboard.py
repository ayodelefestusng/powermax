import os
import io
import pytz
import base64
import logging
from datetime import datetime, date, time as dtime, timedelta
from typing import Tuple, Dict, Any, Optional

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
import matplotlib.ticker as ticker
import numpy as np
from PIL import Image
from sqlalchemy import text

from worker.db import engine

logger = logging.getLogger(__name__)
LAGOS_TZ = pytz.timezone("Africa/Lagos")

def to_lagos(dt: Optional[datetime]) -> Optional[datetime]:
    """Ensure datetime is timezone-aware in Africa/Lagos timezone."""
    if dt is None:
        return None
    if dt.tzinfo is None:
        return LAGOS_TZ.localize(dt)
    return dt.astimezone(LAGOS_TZ)

def format_duration_days_hours(seconds: float) -> str:
    """Format seconds into readable days and hours (e.g., '1d 18h' or '5h 30m')."""
    total_seconds = max(0, int(seconds))
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    days = hours // 24
    rem_hours = hours % 24
    
    if days > 0:
        if rem_hours > 0:
            return f"{days}d {rem_hours}h"
        return f"{days}d"
    elif hours > 0:
        if minutes > 0:
            return f"{hours}h {minutes}m"
        return f"{hours}h"
    else:
        return f"{minutes}m"

def compute_interval_metrics(events_window: list, state_at_start: str, start_dt: datetime, end_dt: datetime) -> Dict[str, Any]:
    """
    Calculate uptime, downtime, and outages in a specific time interval.
    Events window is a list of tuples: (status_upper, server_time_lagos).
    """
    cur_state = state_at_start
    cur_time = start_dt
    
    uptime_sec = 0.0
    downtime_sec = 0.0
    outages = 0
    
    for st, s_time in events_window:
        dur = (s_time - cur_time).total_seconds()
        if dur > 0:
            if cur_state == 'ON':
                uptime_sec += dur
            else:
                downtime_sec += dur
        
        new_state = st
        if cur_state == 'ON' and new_state in ('OFF', 'PARTIAL_OFF'):
            outages += 1
            
        cur_state = new_state
        cur_time = s_time
        
    final_dur = (end_dt - cur_time).total_seconds()
    if final_dur > 0:
        if cur_state == 'ON':
            uptime_sec += final_dur
        else:
            downtime_sec += final_dur
            
    total_sec = uptime_sec + downtime_sec
    uptime_pct = (uptime_sec / total_sec * 100.0) if total_sec > 0 else 0.0
    downtime_pct = (downtime_sec / total_sec * 100.0) if total_sec > 0 else 0.0
    
    return {
        'uptime_sec': uptime_sec,
        'downtime_sec': downtime_sec,
        'uptime_pct': uptime_pct,
        'downtime_pct': downtime_pct,
        'outages': outages
    }

def get_weekly_metrics(feeder_id: int, target_sunday: Optional[date] = None) -> Dict[str, Any]:
    """
    Query database and compute weekly metrics (Sunday to Saturday),
    comparing current week vs last week and computing MTD & WoW metrics.
    Uses optimized single-query event streaming to minimize network overhead.
    """
    now = datetime.now(LAGOS_TZ)
    if target_sunday is None:
        # Determine the most recently completed Saturday
        # weekday(): Monday is 0, Sunday is 6, Saturday is 5
        days_since_sat = (now.weekday() - 5) % 7
        if days_since_sat == 0:
            days_since_sat = 7  # If today is Saturday, report the previous completed week
        sat_date = (now - timedelta(days=days_since_sat)).date()
        sun_date = sat_date - timedelta(days=6)
    else:
        sun_date = target_sunday
        sat_date = sun_date + timedelta(days=6)

    # 1. Define time boundaries in Lagos timezone
    cw_start = LAGOS_TZ.localize(datetime.combine(sun_date, dtime.min))
    cw_end = LAGOS_TZ.localize(datetime.combine(sat_date, dtime.max))
    
    lw_start = cw_start - timedelta(days=7)
    lw_end = cw_end - timedelta(days=7)
    
    mtd_start = LAGOS_TZ.localize(datetime(sat_date.year, sat_date.month, 1, 0, 0, 0))
    mtd_end = cw_end
    
    overall_start = min(lw_start, mtd_start)
    overall_end = cw_end
    
    with engine.connect() as conn:
        feeder_row = conn.execute(
            text("SELECT id, name, band, whatsapp_primary, whatsapp_group, registered_phone FROM myapp_feeder WHERE id = :id"),
            {"id": feeder_id}
        ).fetchone()
        
        if not feeder_row:
            raise ValueError(f"Feeder with id {feeder_id} not found in database.")
            
        pre_row = conn.execute(text("""
            SELECT status, server_time FROM myapp_powerstatus 
            WHERE feeder_id = :id AND server_time < :start_dt 
            ORDER BY server_time DESC LIMIT 1
        """), {"id": feeder_id, "start_dt": overall_start}).fetchone()
        
        event_rows = conn.execute(text("""
            SELECT status, server_time FROM myapp_powerstatus 
            WHERE feeder_id = :id AND server_time >= :start_dt AND server_time <= :end_dt 
            ORDER BY server_time ASC
        """), {"id": feeder_id, "start_dt": overall_start, "end_dt": overall_end}).fetchall()

    feeder_info = {
        "id": feeder_row[0],
        "name": feeder_row[1],
        "band": feeder_row[2] or "A",
        "whatsapp_primary": feeder_row[3],
        "whatsapp_group": feeder_row[4],
        "registered_phone": feeder_row[5],
    }

    events = [(r[0].upper(), to_lagos(r[1])) for r in event_rows]
    initial_state = pre_row[0].upper() if pre_row else 'OFF'
    
    def get_state_before(t_point: datetime) -> str:
        state = initial_state
        for st, st_time in events:
            if st_time < t_point:
                state = st
            else:
                break
        return state
        
    def get_events_between(t_start: datetime, t_end: datetime):
        return [(st, st_time) for st, st_time in events if t_start <= st_time <= t_end]

    # Current Week Metrics
    cw_events = get_events_between(cw_start, cw_end)
    cw_state = get_state_before(cw_start)
    cw_metrics = compute_interval_metrics(cw_events, cw_state, cw_start, cw_end)
    
    # Last Week Metrics
    lw_events = get_events_between(lw_start, lw_end)
    lw_state = get_state_before(lw_start)
    lw_metrics = compute_interval_metrics(lw_events, lw_state, lw_start, lw_end)
    
    # Month-to-Date Metrics
    mtd_events = get_events_between(mtd_start, mtd_end)
    mtd_state = get_state_before(mtd_start)
    mtd_metrics = compute_interval_metrics(mtd_events, mtd_state, mtd_start, mtd_end)
    
    # Daily Breakdown (Sunday to Saturday)
    daily_stats = []
    days_100_pct = 0
    best_day = None
    best_day_pct = -1.0

    for i in range(7):
        d_date = sun_date + timedelta(days=i)
        d_start = LAGOS_TZ.localize(datetime.combine(d_date, dtime.min))
        d_end = LAGOS_TZ.localize(datetime.combine(d_date, dtime.max))
        d_events = get_events_between(d_start, d_end)
        d_state = get_state_before(d_start)
        d_res = compute_interval_metrics(d_events, d_state, d_start, d_end)
        
        pct = d_res['uptime_pct']
        if pct >= 99.5:
            days_100_pct += 1
        if pct > best_day_pct:
            best_day_pct = pct
            best_day = (d_date.strftime("%a"), pct)
            
        daily_stats.append({
            "day": d_date.strftime("%a"),
            "date": d_date.strftime("%d/%m"),
            "full_date": d_date.strftime("%Y-%m-%d"),
            "uptime_pct": round(pct, 1),
            "uptime_hours": round(d_res['uptime_sec'] / 3600.0, 1),
            "downtime_hours": round(d_res['downtime_sec'] / 3600.0, 1),
            "outages": d_res['outages']
        })

    # Week-on-Week comparisons
    wow_uptime_diff = round(cw_metrics['uptime_pct'] - lw_metrics['uptime_pct'], 1)
    wow_downtime_diff_hours = round((cw_metrics['downtime_sec'] - lw_metrics['downtime_sec']) / 3600.0, 1)
    wow_outages_diff = cw_metrics['outages'] - lw_metrics['outages']

    return {
        "feeder": feeder_info,
        "week_range": {
            "start_date": sun_date.strftime("%Y-%m-%d"),
            "end_date": sat_date.strftime("%Y-%m-%d"),
            "display": f"{sun_date.strftime('%b %d')} – {sat_date.strftime('%b %d, %Y')}",
            "short_display": f"{sun_date.strftime('%b %d')} – {sat_date.strftime('%b %d')}"
        },
        "current_week": {
            "uptime_pct": round(cw_metrics['uptime_pct'], 1),
            "uptime_duration": format_duration_days_hours(cw_metrics['uptime_sec']),
            "uptime_hours": round(cw_metrics['uptime_sec'] / 3600.0, 1),
            "downtime_duration": format_duration_days_hours(cw_metrics['downtime_sec']),
            "downtime_hours": round(cw_metrics['downtime_sec'] / 3600.0, 1),
            "outages": cw_metrics['outages']
        },
        "last_week": {
            "uptime_pct": round(lw_metrics['uptime_pct'], 1),
            "uptime_duration": format_duration_days_hours(lw_metrics['uptime_sec']),
            "downtime_duration": format_duration_days_hours(lw_metrics['downtime_sec']),
            "outages": lw_metrics['outages']
        },
        "wow_comparison": {
            "uptime_diff_pct": wow_uptime_diff,
            "uptime_improved": wow_uptime_diff >= 0,
            "uptime_change_str": f"▲ Up from {lw_metrics['uptime_pct']:.1f}%" if wow_uptime_diff >= 0 else f"▼ Down from {lw_metrics['uptime_pct']:.1f}%",
            "downtime_diff_hours": wow_downtime_diff_hours,
            "downtime_improved": wow_downtime_diff_hours <= 0,
            "downtime_change_str": f"▼ Down from {format_duration_days_hours(lw_metrics['downtime_sec'])}" if wow_downtime_diff_hours <= 0 else f"▲ Up from {format_duration_days_hours(lw_metrics['downtime_sec'])}",
            "outages_diff": wow_outages_diff,
            "outages_improved": wow_outages_diff <= 0,
            "outages_change_str": f"▼ Down from {lw_metrics['outages']} incidents" if wow_outages_diff <= 0 else f"▲ Up from {lw_metrics['outages']} incidents",
        },
        "mtd": {
            "uptime_pct": round(mtd_metrics['uptime_pct'], 1),
            "days_with_100_pct": days_100_pct
        },
        "daily_breakdown": daily_stats,
        "best_day": best_day or ("N/A", 0.0)
    }

def generate_whatsapp_summary(metrics: Dict[str, Any]) -> str:
    """
    Format WhatsApp-friendly text summary with emojis and structured markdown.
    """
    feeder = metrics["feeder"]
    cw = metrics["current_week"]
    lw = metrics["last_week"]
    wow = metrics["wow_comparison"]
    mtd = metrics["mtd"]
    week_disp = metrics["week_range"]["display"]
    
    # Emojis and indicators
    up_icon = "🟢" if wow["uptime_improved"] else "🔴"
    wow_sign = "+" if wow["uptime_diff_pct"] >= 0 else ""
    
    lines = [
        "📊 *WEEKLY POWER REPORT SNAPSHOT*",
        "━━━━━━━━━━━━━━━━━━━━━━━",
        f"⚡ *{feeder['name']}* (Band {feeder['band']})",
        f"📅 *{week_disp}*",
        "━━━━━━━━━━━━━━━━━━━━━━━",
        "",
        f"⚡ *Uptime:* {cw['uptime_pct']}% ({cw['uptime_duration']})",
        f"   └ {wow['uptime_change_str']} ({wow_sign}{wow['uptime_diff_pct']}%) {up_icon}",
        "",
        f"⏱️ *Total Downtime:* {cw['downtime_duration']}",
        f"   └ {wow['downtime_change_str']}",
        "",
        f"🔌 *Power Outages:* {cw['outages']} incidents",
        f"   └ {wow['outages_change_str']}",
        "",
        "📊 *Daily Uptime Performance:*",
    ]
    
    # Daily breakdown formatting in compact grid
    daily_items = metrics["daily_breakdown"]
    row1 = " • ".join([f"*{d['day']}:* {d['uptime_pct']}%" for d in daily_items[:4]])
    row2 = " • ".join([f"*{d['day']}:* {d['uptime_pct']}%" for d in daily_items[4:]])
    lines.append(f" {row1}")
    lines.append(f" {row2}")
    lines.append("")
    
    lines.append(f"📈 *Month-to-Date Avg:* {mtd['uptime_pct']}%")
    lines.append(f"🔄 *Week-on-Week Trend:* {wow_sign}{wow['uptime_diff_pct']}% {up_icon}")
    if mtd['days_with_100_pct'] > 0:
        lines.append(f"🏆 *100% Uptime Days:* {mtd['days_with_100_pct']} day{'s' if mtd['days_with_100_pct'] > 1 else ''}")
        
    lines.append("━━━━━━━━━━━━━━━━━━━━━━━")
    lines.append("_Generated automatically by Power Tracker Intelligence_")
    
    return "\n".join(lines)

def generate_infographic_image(metrics: Dict[str, Any], output_path: str) -> str:
    """
    Generate a world-class infographic chart snapshot using Matplotlib,
    matching the attached visual design guide and exported as a lightweight JPG.
    """
    feeder = metrics["feeder"]
    cw = metrics["current_week"]
    wow = metrics["wow_comparison"]
    mtd = metrics["mtd"]
    daily_stats = metrics["daily_breakdown"]
    date_range_str = metrics["week_range"]["display"]
    
    # Use 800 x 1180 px layout at 100 DPI
    fig_w, fig_h = 8.0, 11.8
    fig = plt.figure(figsize=(fig_w, fig_h), dpi=100)
    fig.patch.set_facecolor('#0d3822')
    
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_facecolor('#0d3822')
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')
    
    # Outer frame container
    outer_card = FancyBboxPatch(
        (0.04, 0.03), 0.92, 0.94,
        boxstyle="round,pad=0.01,rounding_size=0.03",
        facecolor='#10452b', edgecolor='#1a5e3b',
        linewidth=1.5, zorder=1
    )
    ax.add_patch(outer_card)

    # 1. Header Section
    ax.text(
        0.5, 0.938, "⚡ Weekly Power Report Snapshot",
        ha='center', va='center',
        fontsize=18, fontweight='bold', color='#ffffff',
        zorder=3
    )
    
    # Feeder Name & Band Badge
    badge_w = min(0.70, max(0.45, len(feeder['name']) * 0.02 + 0.20))
    badge_x = (1.0 - badge_w) / 2.0
    badge_box = FancyBboxPatch(
        (badge_x, 0.888), badge_w, 0.032,
        boxstyle="round,pad=0.005,rounding_size=0.015",
        facecolor='#0b2b1a', edgecolor='#22c55e',
        linewidth=1.0, zorder=2
    )
    ax.add_patch(badge_box)
    
    ax.text(
        0.5, 0.904, f"{feeder['name']}  •  Band {feeder['band']}",
        ha='center', va='center',
        fontsize=11.5, fontweight='bold', color='#4ade80',
        zorder=3
    )
    
    ax.text(
        0.5, 0.866, f"{date_range_str}",
        ha='center', va='center',
        fontsize=11, fontweight='normal', color='#cbd5e1',
        zorder=3
    )

    # 2. Three Metric Cards (Uptime, Downtime, Outages)
    card_y = 0.675
    card_h = 0.165
    card_w = 0.28
    gap = 0.025
    start_x = 0.065
    
    cards_data = [
        {
            "x": start_x,
            "title": "⚡ Uptime",
            "val": f"{cw['uptime_pct']:.1f}%",
            "sub": cw['uptime_duration'],
            "badge": wow['uptime_change_str'],
            "badge_color": '#15803d' if wow['uptime_improved'] else '#b91c1c',
            "accent": '#4ade80'
        },
        {
            "x": start_x + card_w + gap,
            "title": "Downtime",
            "val": cw['downtime_duration'],
            "sub": f"{100 - cw['uptime_pct']:.1f}% off-grid",
            "badge": wow['downtime_change_str'],
            "badge_color": '#15803d' if wow['downtime_improved'] else '#b91c1c',
            "accent": '#fbbf24'
        },
        {
            "x": start_x + 2 * (card_w + gap),
            "title": "Outages",
            "val": f"{cw['outages']}",
            "sub": "incidents",
            "badge": wow['outages_change_str'],
            "badge_color": '#15803d' if wow['outages_improved'] else '#b91c1c',
            "accent": '#f87171'
        }
    ]

    for c in cards_data:
        c_box = FancyBboxPatch(
            (c["x"], card_y), card_w, card_h,
            boxstyle="round,pad=0.008,rounding_size=0.02",
            facecolor='#0b2e1d', edgecolor='#1d6e43',
            linewidth=1.2, zorder=2
        )
        ax.add_patch(c_box)
        
        ax.text(
            c["x"] + card_w/2, card_y + card_h - 0.025, c["title"],
            ha='center', va='center',
            fontsize=11.5, fontweight='bold', color=c["accent"],
            zorder=3
        )
        
        ax.text(
            c["x"] + card_w/2, card_y + card_h - 0.065, c["val"],
            ha='center', va='center',
            fontsize=18, fontweight='bold', color='#ffffff',
            zorder=3
        )
        
        ax.text(
            c["x"] + card_w/2, card_y + card_h - 0.098, c["sub"],
            ha='center', va='center',
            fontsize=9.5, fontweight='normal', color='#94a3b8',
            zorder=3
        )
        
        pill_w = card_w - 0.028
        pill_h = 0.032
        pill_x = c["x"] + 0.014
        pill_y = card_y + 0.015
        pill_box = FancyBboxPatch(
            (pill_x, pill_y), pill_w, pill_h,
            boxstyle="round,pad=0.004,rounding_size=0.012",
            facecolor=c["badge_color"], edgecolor='none',
            zorder=3
        )
        ax.add_patch(pill_box)
        ax.text(
            c["x"] + card_w/2, pill_y + pill_h/2, c["badge"],
            ha='center', va='center',
            fontsize=8.2, fontweight='bold', color='#ffffff',
            zorder=4
        )

    # 3. Section Title: Daily Performance
    ax.text(
        0.5, 0.628, "Daily Performance Breakdown",
        ha='center', va='center',
        fontsize=13, fontweight='bold', color='#e2e8f0',
        zorder=3
    )
    ax.plot([0.15, 0.85], [0.612, 0.612], color='#1d6e43', lw=1.0, zorder=2)

    # 4. Bar Chart Sub-Axes
    chart_ax = fig.add_axes([0.10, 0.225, 0.80, 0.355])
    chart_ax.set_facecolor('#0b2e1d')
    
    for spine in chart_ax.spines.values():
        spine.set_color('#1d6e43')
        spine.set_linewidth(1.0)
        
    days = [s["day"] for s in daily_stats]
    pcts = [s["uptime_pct"] for s in daily_stats]
    dates = [s["date"] for s in daily_stats]
    x_pos = np.arange(len(days))
    
    max_pct = max(pcts) if pcts else 100
    y_lim = min(100.0, max(max_pct * 1.25, 35.0))
    chart_ax.set_ylim(0, y_lim)
    chart_ax.set_xlim(-0.6, len(days) - 0.4)
    
    chart_ax.grid(axis='y', linestyle='--', alpha=0.25, color='#22c55e', zorder=1)
    chart_ax.set_axisbelow(True)
    
    bar_width = 0.55
    bars = chart_ax.bar(
        x_pos, pcts, width=bar_width,
        color='#22c55e', edgecolor='#4ade80',
        linewidth=1.2, zorder=3,
        alpha=0.92
    )
    
    # Value labels above bars
    for bar, p in zip(bars, pcts):
        h = bar.get_height()
        chart_ax.text(
            bar.get_x() + bar.get_width() / 2.0, h + (y_lim * 0.025),
            f"{p:.1f}%",
            ha='center', va='bottom',
            fontsize=9.5, fontweight='bold', color='#ffffff',
            zorder=4
        )
        
    # Average reference line across chart
    cw_pct = cw['uptime_pct']
    chart_ax.axhline(
        y=cw_pct, color='#facc15', linestyle=':',
        linewidth=1.5, zorder=4
    )
    # Position reference text neatly on the top left or above line
    chart_ax.text(
        0.02, 0.95,
        f"--- Weekly Avg: {cw_pct:.1f}%",
        transform=chart_ax.transAxes,
        ha='left', va='top',
        fontsize=9.0, fontweight='bold', color='#facc15',
        zorder=5,
        bbox=dict(boxstyle="round,pad=0.2", facecolor='#0b2b1a', edgecolor='#facc15', lw=0.8, alpha=0.85)
    )

    # X-axis ticks with Day + Date
    tick_labels = [f"{day}\n{dt}" for day, dt in zip(days, dates)]
    chart_ax.set_xticks(x_pos)
    chart_ax.set_xticklabels(tick_labels, fontsize=9.5, fontweight='bold', color='#cbd5e1')
    chart_ax.tick_params(axis='x', length=0, pad=6)
    
    chart_ax.tick_params(axis='y', colors='#94a3b8', labelsize=8.5)
    chart_ax.yaxis.set_major_formatter(ticker.PercentFormatter(xmax=100, decimals=0))

    # 5. Bottom Summary Pills
    bottom_box = FancyBboxPatch(
        (0.065, 0.082), 0.87, 0.095,
        boxstyle="round,pad=0.008,rounding_size=0.02",
        facecolor='#0b2b1a', edgecolor='#1d6e43',
        linewidth=1.2, zorder=2
    )
    ax.add_patch(bottom_box)
    
    wow_diff = wow["uptime_diff_pct"]
    wow_sign = "+" if wow_diff >= 0 else ""
    wow_arrow = "▲" if wow_diff >= 0 else "▼"
    
    best_day_name, best_day_val = metrics.get("best_day", ("N/A", 0.0))
    best_day_str = f"{best_day_name} ({best_day_val:.1f}%)" if best_day_name != "N/A" else "N/A"
    
    ax.text(
        0.5, 0.146,
        f"Month-to-Date Avg: {mtd['uptime_pct']:.1f}%    •    Week-on-Week: {wow_sign}{wow_diff:.1f}% {wow_arrow}",
        ha='center', va='center',
        fontsize=10.5, fontweight='bold', color='#f1f5f9',
        zorder=3
    )
    
    days_100 = mtd.get("days_with_100_pct", 0)
    top_extra = f"100% Uptime: {days_100} day{'s' if days_100 != 1 else ''}" if days_100 > 0 else f"Band {feeder['band']} Target Tracking"
    
    ax.text(
        0.5, 0.108,
        f"Top Day: {best_day_str}    •    {top_extra}",
        ha='center', va='center',
        fontsize=9.5, fontweight='normal', color='#94a3b8',
        zorder=3
    )

    # 6. Footer branding
    ax.text(
        0.5, 0.046,
        "Power Tracker Intelligence  •  Weekly Snapshot System",
        ha='center', va='center',
        fontsize=8.5, color='#4ade80', alpha=0.7,
        zorder=3
    )

    # Ensure output directory exists
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    
    # Save optimized JPEG (100 DPI, 75% quality for lightweight WhatsApp sending)
    buf = io.BytesIO()
    plt.savefig(
        buf, format='jpeg', dpi=100,
        bbox_inches='tight', pad_inches=0.1,
        pil_kwargs={"quality": 75, "optimize": True}
    )
    plt.close(fig)
    
    with open(output_path, "wb") as f:
        f.write(buf.getvalue())
        
    kb_size = len(buf.getvalue()) / 1024.0
    logger.info(f"Infographic saved to {output_path} ({kb_size:.1f} KB)")
    return output_path

def generate_weekly_power_infographic(
    feeder_id: int,
    target_sunday: Optional[date] = None,
    output_dir: Optional[str] = None
) -> Tuple[str, str, Dict[str, Any]]:
    """
    Main entry point:
    1. Computes all weekly metrics.
    2. Formats WhatsApp text summary.
    3. Generates lightweight infographic JPG.
    Returns: (text_summary, image_filename, metrics_dict).
    """
    metrics = get_weekly_metrics(feeder_id=feeder_id, target_sunday=target_sunday)
    text_summary = generate_whatsapp_summary(metrics)
    
    if output_dir is None:
        output_dir = os.path.join(os.path.dirname(__file__), "generated_reports")
    os.makedirs(output_dir, exist_ok=True)
    
    safe_name = "".join(c for c in metrics["feeder"]["name"] if c.isalnum() or c in ('_', '-')).strip()
    week_str = metrics["week_range"]["start_date"]
    filename = f"weekly_snapshot_{feeder_id}_{safe_name}_{week_str}.jpg"
    full_path = os.path.join(output_dir, filename)
    
    image_filename = generate_infographic_image(metrics, full_path)
    return text_summary, image_filename, metrics
