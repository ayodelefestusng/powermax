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
from matplotlib.patches import FancyBboxPatch, Rectangle
import matplotlib.ticker as ticker
import numpy as np
from PIL import Image
from sqlalchemy import text

from worker.db import engine
from worker.logging_config import get_logger

logger = get_logger(__name__)
LAGOS_TZ = pytz.timezone("Africa/Lagos")

BAND_MIN_HOURS = {
    'A': 20.0,
    'B': 16.0,
    'C': 12.0,
    'D': 8.0,
    'E': 4.0,
    'F': 0.0
}

def to_lagos(dt: Optional[datetime]) -> Optional[datetime]:
    """Ensure datetime is timezone-aware in Africa/Lagos timezone."""
    if dt is None:
        return None
    if dt.tzinfo is None:
        return LAGOS_TZ.localize(dt)
    return dt.astimezone(LAGOS_TZ)

def format_duration_days_hours(seconds: float) -> str:
    """Format seconds into readable days and hours (e.g., '1d 18h' or '0d 6h')."""
    total_seconds = max(0, int(seconds))
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    days = hours // 24
    rem_hours = hours % 24
    
    if days > 0:
        if rem_hours > 0:
            return f"{days}d {rem_hours}h"
        return f"{days}d 0h"
    elif hours > 0:
        return f"0d {hours}h"
    else:
        return f"0d 0h"

def format_duration_hours_mins(seconds: float) -> str:
    """Format seconds into readable hours and minutes (e.g., '6 hrs 12 mins')."""
    total_seconds = max(0, int(seconds))
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    return f"{hours} hrs {minutes} mins"

def format_duration_hrs_mins_colon(seconds: float) -> str:
    """Format seconds into readable hours and minutes (e.g., '6hrs: 20mins')."""
    total_mins = int(round(max(0.0, seconds) / 60.0))
    hours = total_mins // 60
    minutes = total_mins % 60
    h_str = "hr" if hours == 1 else "hrs"
    m_str = "min" if minutes == 1 else "mins"
    return f"{hours}{h_str}: {minutes}{m_str}"

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
    """
    now = datetime.now(LAGOS_TZ)
    if target_sunday is None:
        days_since_sat = (now.weekday() - 5) % 7
        if days_since_sat == 0:
            days_since_sat = 7
        sat_date = (now - timedelta(days=days_since_sat)).date()
        sun_date = sat_date - timedelta(days=6)
    else:
        sun_date = target_sunday
        sat_date = sun_date + timedelta(days=6)

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

    cw_events = get_events_between(cw_start, cw_end)
    cw_state = get_state_before(cw_start)
    cw_metrics = compute_interval_metrics(cw_events, cw_state, cw_start, cw_end)
    
    lw_events = get_events_between(lw_start, lw_end)
    lw_state = get_state_before(lw_start)
    lw_metrics = compute_interval_metrics(lw_events, lw_state, lw_start, lw_end)
    
    mtd_events = get_events_between(mtd_start, mtd_end)
    mtd_state = get_state_before(mtd_start)
    mtd_metrics = compute_interval_metrics(mtd_events, mtd_state, mtd_start, mtd_end)
    
    daily_stats = []
    days_100_pct = 0
    best_day_name = None
    best_day_sec = 0.0
    best_day_pct = -1.0

    for i in range(7):
        d_date = sun_date + timedelta(days=i)
        d_start = LAGOS_TZ.localize(datetime.combine(d_date, dtime.min))
        d_end = LAGOS_TZ.localize(datetime.combine(d_date, dtime.max))
        d_events = get_events_between(d_start, d_end)
        d_state = get_state_before(d_start)
        d_res = compute_interval_metrics(d_events, d_state, d_start, d_end)
        
        pct = d_res['uptime_pct']
        up_sec = d_res['uptime_sec']
        if pct >= 99.5:
            days_100_pct += 1
        if pct > best_day_pct:
            best_day_pct = pct
            best_day_sec = up_sec
            best_day_name = d_date.strftime("%a")
            
        daily_stats.append({
            "day": d_date.strftime("%a"),
            "date": d_date.strftime("%d/%m"),
            "full_date": d_date.strftime("%a %b %d, %Y"),
            "avail": format_duration_hours_mins(up_sec),
            "pct": round(pct, 1),
            "uptime_pct": round(pct, 1),
            "uptime_hours": round(up_sec / 3600.0, 1),
            "downtime_hours": round(d_res['downtime_sec'] / 3600.0, 1),
            "outages": d_res['outages']
        })

    wow_uptime_diff = round(cw_metrics['uptime_pct'] - lw_metrics['uptime_pct'], 1)
    wow_downtime_diff_hours = round((cw_metrics['downtime_sec'] - lw_metrics['downtime_sec']) / 3600.0, 1)
    wow_outages_diff = cw_metrics['outages'] - lw_metrics['outages']

    top_day_duration_str = format_duration_days_hours(best_day_sec)
    top_day_formatted = f"⭐ {best_day_name} ({top_day_duration_str} • {best_day_pct:.1f}%)" if best_day_name else "N/A"

    band_code = str(feeder_info['band']).strip().upper().replace("BAND", "").strip() or 'D'
    min_required_hours = BAND_MIN_HOURS.get(band_code, 8.0)
    avg_daily_hours = (cw_metrics['uptime_sec'] / 7.0) / 3600.0
    grid_target_met = avg_daily_hours >= min_required_hours

    mtd_avg_daily_sec = (mtd_metrics['uptime_pct'] / 100.0) * 86400.0
    mtd_duration_str = format_duration_hrs_mins_colon(mtd_avg_daily_sec)

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
            "uptime_change_str": f"▲ Up from {lw_metrics['uptime_pct']:.1f}% last week" if wow_uptime_diff >= 0 else f"▼ Down from {lw_metrics['uptime_pct']:.1f}% last week",
            "downtime_diff_hours": wow_downtime_diff_hours,
            "downtime_improved": wow_downtime_diff_hours <= 0,
            "downtime_change_str": f"▼ Down {abs(wow_downtime_diff_hours)}h vs last week" if wow_downtime_diff_hours <= 0 else f"▲ Up {wow_downtime_diff_hours}h vs last week",
            "outages_diff": wow_outages_diff,
            "outages_improved": wow_outages_diff <= 0,
            "outages_change_str": f"▼ Down from {lw_metrics['outages']} inc." if wow_outages_diff <= 0 else f"▲ Up from {lw_metrics['outages']} inc.",
        },
        "mtd": {
            "uptime_pct": round(mtd_metrics['uptime_pct'], 1),
            "uptime_duration": mtd_duration_str,
            "avg_daily_hours": round(mtd_avg_daily_sec / 3600.0, 1),
            "days_with_100_pct": days_100_pct
        },
        "grid_target": {
            "band": band_code,
            "min_required_hours": min_required_hours,
            "avg_daily_hours": round(avg_daily_hours, 1),
            "is_met": grid_target_met,
            "status_text": "Minimum Met" if grid_target_met else "Minimum Not Met"
        },
        "daily_breakdown": daily_stats,
        "top_day_str": top_day_formatted
    }

def generate_whatsapp_summary(metrics: Dict[str, Any]) -> str:
    feeder = metrics["feeder"]
    cw = metrics["current_week"]
    wow = metrics["wow_comparison"]
    mtd = metrics["mtd"]
    gt = metrics["grid_target"]
    week_disp = metrics["week_range"]["display"]
    
    up_icon = "🟢" if wow["uptime_improved"] else "🔴"
    down_icon = "🟢" if wow["downtime_improved"] else "🔴"
    outage_icon = "🟢" if wow["outages_improved"] else "🔴"
    wow_sign = "+" if wow["uptime_diff_pct"] >= 0 else ""
    target_emoji = "✅" if gt["is_met"] else "❌"
    
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
        f"⏱️ *Total Downtime:* {cw['downtime_duration']} ({100 - cw['uptime_pct']:.1f}% off-grid)",
        f"   └ {wow['downtime_change_str']} {down_icon}",
        "",
        f"🔌 *Power Outages:* {cw['outages']} incidents",
        f"   └ {wow['outages_change_str']} {outage_icon}",
        "",
        f"🎯 *Grid Target:* {target_emoji} *{gt['status_text']}* (Band {gt['band']}: {gt['min_required_hours']}h/day req.)",
        f"🏆 *Top Supply Day:* {metrics['top_day_str']}",
        "",
        "📋 *Daily Performance Table:*",
        "```",
        f"{'Date':<17} | {'Availability':<12} | {'% Uptime':<8} | {'Outages':<7}",
        "-" * 50,
    ]
    
    for d in metrics["daily_breakdown"]:
        lines.append(f"{d['full_date']:<17} | {d['avail']:<12} | {d['pct']:>6.1f}% | {d['outages']:^7}")
        
    mtd_dur_str = f" ({mtd['uptime_duration']})" if mtd.get("uptime_duration") else ""

    lines.extend([
        "```",
        "",
        f"📈 *Month-to-Date Avg:* {mtd['uptime_pct']}%{mtd_dur_str}",
        f"🔄 *Week-on-Week Trend:* {wow_sign}{wow['uptime_diff_pct']}% {up_icon}",
        "━━━━━━━━━━━━━━━━━━━━━━━",
        "_Generated automatically by Power Tracker Intelligence_"
    ])
    
    return "\n".join(lines)

def generate_infographic_image(metrics: Dict[str, Any], output_path: str) -> str:
    """
    Generate an enhanced infographic chart snapshot using Matplotlib,
    incorporating dynamic color gradients, availability bar labels, grid benchmark line,
    and high-contrast typography. Exported as an ultra-compact JPEG (~85-95 KB).
    """
    feeder = metrics["feeder"]
    cw = metrics["current_week"]
    wow = metrics["wow_comparison"]
    mtd = metrics["mtd"]
    gt = metrics["grid_target"]
    daily_stats = metrics["daily_breakdown"]
    date_range_str = metrics["week_range"]["display"]
    
    # Dimensions: 8.2 x 13.0 inches at 100 DPI -> 820 x 1300 px
    fig_w, fig_h = 8.2, 13.0
    fig = plt.figure(figsize=(fig_w, fig_h), dpi=100)
    fig.patch.set_facecolor('#f8fafc')
    
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_facecolor('#f8fafc')
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')
    
    # Outer main white card
    card_margin_x = 0.035
    card_margin_y = 0.02
    card_w = 1.0 - 2 * card_margin_x
    card_h = 1.0 - 2 * card_margin_y
    
    main_card = FancyBboxPatch(
        (card_margin_x, card_margin_y), card_w, card_h,
        boxstyle="round,pad=0.008,rounding_size=0.025",
        facecolor='#ffffff', edgecolor='#e2e8f0',
        linewidth=1.5, zorder=1
    )
    ax.add_patch(main_card)
    
    # 1. Header Section
    ax.text(
        0.5, 0.958, "⚡ Weekly Power Report Snapshot",
        ha='center', va='center',
        fontsize=18, fontweight='bold', color='#0f172a',
        zorder=3
    )
    
    # Feeder Name in Bold Green for Stronger Identity
    band_clean = str(feeder['band']).replace('Band', '').strip()
    feeder_badge_text = f"{feeder['name']}  •  Band {band_clean}"
    badge_w = min(0.72, max(0.45, len(feeder_badge_text) * 0.018 + 0.18))
    badge_x = (1.0 - badge_w) / 2.0
    badge_box = FancyBboxPatch(
        (badge_x, 0.915), badge_w, 0.028,
        boxstyle="round,pad=0.004,rounding_size=0.012",
        facecolor='#f0fdf4', edgecolor='#86efac',
        linewidth=1.2, zorder=2
    )
    ax.add_patch(badge_box)
    ax.text(
        0.5, 0.929, feeder_badge_text,
        ha='center', va='center',
        fontsize=12, fontweight='heavy', color='#047857',
        zorder=3
    )
    
    # Date Subtitle
    ax.text(
        0.5, 0.888, f"{date_range_str}",
        ha='center', va='center',
        fontsize=11, fontweight='bold', color='#64748b',
        zorder=3
    )
    
    ax.plot([0.08, 0.92], [0.870, 0.870], color='#f1f5f9', lw=1.5, zorder=2)

    # 2. Three Metric Cards (Uptime, Downtime, Outages) with Durations & Percentages
    card_y = 0.718
    card_h = 0.134
    metric_w = 0.275
    gap = 0.022
    start_x = 0.065
    
    cards_data = [
        {
            "x": start_x,
            "title": "⚡ Uptime:",
            "val": f"{cw['uptime_pct']:.1f}%",
            "val_color": '#15803d',
            "sub": f"({cw['uptime_duration']})",
            "badge": wow['uptime_change_str'],
            "badge_bg": '#16a34a' if wow['uptime_improved'] else '#dc2626',
        },
        {
            "x": start_x + metric_w + gap,
            "title": "⏱️ Downtime:",
            "val": cw['downtime_duration'],
            "val_color": '#1e3a8a',
            "sub": f"({100 - cw['uptime_pct']:.1f}% off-grid)",
            "badge": wow['downtime_change_str'],
            "badge_bg": '#16a34a' if wow['downtime_improved'] else '#dc2626',
        },
        {
            "x": start_x + 2 * (metric_w + gap),
            "title": "🔌 Outages:",
            "val": f"{cw['outages']} incidents",
            "val_color": '#b91c1c',
            "sub": "total power cuts",
            "badge": wow['outages_change_str'],
            "badge_bg": '#16a34a' if wow['outages_improved'] else '#dc2626',
        }
    ]

    for c in cards_data:
        c_box = FancyBboxPatch(
            (c["x"], card_y), metric_w, card_h,
            boxstyle="round,pad=0.006,rounding_size=0.018",
            facecolor='#ffffff', edgecolor='#e2e8f0',
            linewidth=1.2, zorder=2
        )
        ax.add_patch(c_box)
        
        ax.text(
            c["x"] + metric_w/2, card_y + card_h - 0.022, c["title"],
            ha='center', va='center',
            fontsize=11.5, fontweight='bold', color='#0f172a',
            zorder=3
        )
        
        ax.text(
            c["x"] + metric_w/2, card_y + card_h - 0.054, c["val"],
            ha='center', va='center',
            fontsize=16, fontweight='bold', color=c["val_color"],
            zorder=3
        )
        
        ax.text(
            c["x"] + metric_w/2, card_y + card_h - 0.080, c["sub"],
            ha='center', va='center',
            fontsize=9.0, fontweight='bold', color='#64748b',
            zorder=3
        )
        
        pill_w = metric_w - 0.024
        pill_h = 0.027
        pill_x = c["x"] + 0.012
        pill_y = card_y + 0.012
        pill_box = FancyBboxPatch(
            (pill_x, pill_y), pill_w, pill_h,
            boxstyle="round,pad=0.003,rounding_size=0.008",
            facecolor=c["badge_bg"], edgecolor='none',
            zorder=3
        )
        ax.add_patch(pill_box)
        ax.text(
            c["x"] + metric_w/2, pill_y + pill_h/2, c["badge"],
            ha='center', va='center',
            fontsize=8.0, fontweight='bold', color='#ffffff',
            zorder=4
        )

    # 3. Section Title: Daily Performance
    ax.text(
        0.5, 0.680, "Daily Performance & Availability",
        ha='center', va='center',
        fontsize=13, fontweight='bold', color='#0f172a',
        zorder=3
    )
    ax.plot([0.30, 0.70], [0.668, 0.668], color='#e2e8f0', lw=1.2, zorder=2)

    # 4. Bar Chart Sub-Axes (Dynamic Gradient & Availability Labels)
    chart_ax = fig.add_axes([0.10, 0.280, 0.80, 0.365])
    chart_ax.set_facecolor('#ffffff')
    
    for spine in ['top', 'right', 'left']:
        chart_ax.spines[spine].set_visible(False)
    chart_ax.spines['bottom'].set_color('#cbd5e1')
    chart_ax.spines['bottom'].set_linewidth(1.0)
    
    days = [s["day"] for s in daily_stats]
    pcts = [s["pct"] for s in daily_stats]
    avails = [s["avail"] for s in daily_stats]
    x_pos = np.arange(len(days))
    
    max_pct = max(pcts) if pcts else 100
    y_lim = min(100.0, max(max_pct * 1.25, 40.0))
    chart_ax.set_ylim(0, y_lim)
    chart_ax.set_xlim(-0.6, len(days) - 0.4)
    
    chart_ax.grid(axis='y', linestyle=':', alpha=0.6, color='#cbd5e1', zorder=1)
    chart_ax.set_axisbelow(True)
    
    # Dynamic Color Gradient: Green >80%, Amber 40-80%, Red <40%
    bar_colors = []
    bar_edges = []
    for p in pcts:
        if p >= 80.0:
            bar_colors.append('#22c55e')
            bar_edges.append('#16a34a')
        elif p >= 40.0:
            bar_colors.append('#f59e0b')
            bar_edges.append('#d97706')
        else:
            bar_colors.append('#ef4444')
            bar_edges.append('#dc2626')

    bar_width = 0.52
    bars = chart_ax.bar(
        x_pos, pcts, width=bar_width,
        color=bar_colors, edgecolor=bar_edges,
        linewidth=1.2, zorder=3,
        alpha=0.90
    )
    
    # Render Availability (hr min) and % labels above bars
    for bar, p, avail in zip(bars, pcts, avails):
        h = bar.get_height()
        chart_ax.text(
            bar.get_x() + bar.get_width() / 2.0, h + (y_lim * 0.03),
            f"{avail}\n({p:.1f}%)",
            ha='center', va='bottom',
            fontsize=8.5, fontweight='bold', color='#1e293b',
            zorder=4
        )
        
    # Grid Target Benchmark Line
    target_pct = (gt['min_required_hours'] / 24.0) * 100.0
    chart_ax.axhline(
        y=target_pct, color='#dc2626', linestyle='--',
        linewidth=1.4, zorder=2
    )
    chart_ax.text(
        len(days) - 0.5, target_pct + 2.5,
        f"Grid Target ({gt['min_required_hours']}h/day)",
        ha='right', va='bottom',
        fontsize=8.5, fontweight='bold', color='#dc2626',
        zorder=4,
        bbox=dict(boxstyle="round,pad=0.2", facecolor='#fef2f2', edgecolor='#fca5a5', lw=0.8, alpha=0.9)
    )

    chart_ax.set_xticks(x_pos)
    chart_ax.set_xticklabels(days, fontsize=10.0, fontweight='bold', color='#334155')
    chart_ax.tick_params(axis='x', length=0, pad=6)
    chart_ax.tick_params(axis='y', colors='#94a3b8', labelsize=8.5)
    chart_ax.yaxis.set_major_formatter(ticker.PercentFormatter(xmax=100, decimals=0))

    # 5. BOTTOM SUMMARY SECTION
    bottom_y = 0.055
    bottom_h = 0.115
    bottom_box = FancyBboxPatch(
        (0.065, bottom_y), 0.87, bottom_h,
        boxstyle="round,pad=0.006,rounding_size=0.018",
        facecolor='#f8fafc', edgecolor='#e2e8f0',
        linewidth=1.2, zorder=2
    )
    ax.add_patch(bottom_box)
    
    wow_diff = wow["uptime_diff_pct"]
    wow_sign = "+" if wow_diff >= 0 else ""
    mtd_dur_str = f" ({mtd['uptime_duration']})" if mtd.get("uptime_duration") else ""
    
    ax.text(
        0.5, bottom_y + bottom_h - 0.032,
        f"Month-to-Date Avg: {mtd['uptime_pct']:.1f}%{mtd_dur_str}    •    Week-on-Week: {wow_sign}{wow_diff:.1f}%",
        ha='center', va='center',
        fontsize=9.8, fontweight='bold', color='#0f172a',
        zorder=3
    )
    
    target_color = '#15803d' if gt['is_met'] else '#dc2626'
    
    ax.text(
        0.28, bottom_y + 0.035,
        f"Top Day: {metrics['top_day_str']}",
        ha='center', va='center',
        fontsize=9.2, fontweight='bold', color='#047857',
        zorder=3
    )
    
    ax.text(
        0.52, bottom_y + 0.035,
        "•",
        ha='center', va='center',
        fontsize=10.0, fontweight='bold', color='#94a3b8',
        zorder=3
    )
    
    ax.text(
        0.74, bottom_y + 0.035,
        f"Grid Target: {gt['status_text']}",
        ha='center', va='center',
        fontsize=9.5, fontweight='bold', color=target_color,
        zorder=3
    )

    # 6. Footer
    ax.text(
        0.5, 0.022,
        "Power Tracker Intelligence  •  Automated Weekly Snapshot System",
        ha='center', va='center',
        fontsize=8.5, color='#94a3b8',
        zorder=3
    )

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    
    buf = io.BytesIO()
    plt.savefig(
        buf, format='jpeg', dpi=100,
        bbox_inches='tight', pad_inches=0.1,
        pil_kwargs={"quality": 80, "optimize": True}
    )
    plt.close(fig)
    
    with open(output_path, "wb") as f:
        f.write(buf.getvalue())
        
    kb_size = len(buf.getvalue()) / 1024.0
    logger.info(f"Enhanced Infographic saved to {output_path} ({kb_size:.1f} KB)")
    return output_path

def generate_weekly_power_infographic(
    feeder_id: int,
    target_sunday: Optional[date] = None,
    output_dir: Optional[str] = None
) -> Tuple[str, str, Dict[str, Any]]:
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