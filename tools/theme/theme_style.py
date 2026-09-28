"""
7explorer theme style: fully authored visual definition (no external
assets, no Microsoft-derived values). Generates the VARIANT property
stream for our own Win7-shell-oriented classes. Everything colour-based;
no images are referenced in v1 (uxtheme draws gradients from colour
records).
"""
from build_theme import (Rec, T_COLOR, T_INT, T_BOOL, T_STRING,
                         T_MARGINS, rgb, PALETTE,
                         TMT_FILLCOLOR, TMT_TEXTCOLOR, TMT_HEIGHT,
                         TMT_SIZINGMARGINS, TMT_CONTENTMARGINS,
                         TMT_FLATMENUS)

# class names the Win7 shell asks uxtheme for (functional identifiers,
# case matches Win7 OpenThemeData callers)
CLS7 = ["GLOBALS", "Taskbar", "TaskBand", "Rebar", "StartPanel",
        "TrayNotify", "Clock", "Menu", "MenuBand", "StartMenu",
        "TaskBar::Taskbar", "Toolbar", "Tooltip"]

# our authored palette (original values, aero-flavoured)
C_TASKBAR_TOP = rgb(0x6C, 0xA3, 0xD8)
C_TASKBAR_BOT = rgb(0x3A, 0x66, 0x9C)
C_TASKBAR_TXT = rgb(0xFF, 0xFF, 0xFF)
C_MENU_BG = rgb(0xF2, 0xF5, 0xF9)
C_MENU_HOT = rgb(0x4F, 0x83, 0xBB)
C_MENU_TXT = rgb(0x1E, 0x2B, 0x3C)
C_REBAR_BG = rgb(0x51, 0x7D, 0xAF)
C_CLOCK_TXT = rgb(0xFF, 0xFF, 0xFF)
C_START_BG = rgb(0x51, 0x7D, 0xAF)
C_TRAY_BG = rgb(0x4A, 0x75, 0xA8)

# tmschema property ids (msstyles v2 schema, previously confirmed)
TMT_GRADIENTCOLOR1 = 3811
TMT_GRADIENTCOLOR2 = 3812
TMT_BORDERSIZE = 2403
TMT_SIZINGTYPE = 201  # ST_TILE etc (enum)

# part ids
TBP_BOTTOM, TBP_RIGHT, TBP_TOP, TBP_LEFT = 1, 2, 3, 4
RP_GRIPPER, RP_GRIPPERVERT, RP_BAND, RP_CHEVRON, RP_CHEVRONVERT = 1, 2, 3, 4, 5
TNP_BACKGROUND = 1
CLP_TIME = 1
MP_MENUITEM, MP_MENUDROPDOWN, MP_MENUBARITEM, MP_MENUBARDROPDOWN = 1, 2, 3, 4
MP_CHEVRON, MP_SEPARATOR = 5, 6


def make_style_records(cid_of):
    """cid_of: class name -> CMAP index"""
    recs = []
    R = recs.append

    # ---- reserved variant classes (loader requires records here) ----
    for nm in ("sizevariant.NormalSize", "sizevariant.Default",
               "colorvariant.NormalColor", "globals", "sysmetrics"):
        cid = cid_of(nm)
        if cid is not None:
            R(Rec(TMT_FILLCOLOR, T_COLOR, cid, 0, 0, C_TASKBAR_BOT))

    # ---- globals ----
    g = cid_of("GLOBALS")
    if g is not None:
        R(Rec(TMT_FLATMENUS, T_BOOL, g, 0, 0, 1))

    # ---- taskbar backgrounds (all four orientations) ----
    tb = cid_of("Taskbar")
    if tb is not None:
        for part in (TBP_BOTTOM, TBP_RIGHT, TBP_TOP, TBP_LEFT):
            R(Rec(TMT_FILLCOLOR, T_COLOR, tb, part, 0, C_TASKBAR_BOT))
            R(Rec(TMT_GRADIENTCOLOR1, T_COLOR, tb, part, 0, C_TASKBAR_TOP))
            R(Rec(TMT_GRADIENTCOLOR2, T_COLOR, tb, part, 0, C_TASKBAR_BOT))
            R(Rec(TMT_HEIGHT, T_INT, tb, part, 0, 30))
            R(Rec(TMT_SIZINGMARGINS, T_MARGINS, tb, part, 0, (4, 4, 4, 4)))
            R(Rec(TMT_TEXTCOLOR, T_COLOR, tb, part, 0, C_TASKBAR_TXT))

    # ---- rebar ----
    rb = cid_of("Rebar")
    if rb is not None:
        for part in (RP_GRIPPER, RP_GRIPPERVERT, RP_BAND, RP_CHEVRON,
                     RP_CHEVRONVERT):
            R(Rec(TMT_FILLCOLOR, T_COLOR, rb, part, 0, C_REBAR_BG))

    # ---- tray, clock ----
    tn = cid_of("TrayNotify")
    if tn is not None:
        R(Rec(TMT_FILLCOLOR, T_COLOR, tn, TNP_BACKGROUND, 0, C_TRAY_BG))
    ck = cid_of("Clock")
    if ck is not None:
        R(Rec(TMT_TEXTCOLOR, T_COLOR, ck, CLP_TIME, 0, C_CLOCK_TXT))
        R(Rec(TMT_FILLCOLOR, T_COLOR, ck, CLP_TIME, 0, C_TRAY_BG))

    # ---- start panel ----
    sp = cid_of("StartPanel")
    if sp is not None:
        for part, col in ((1, C_START_BG), (4, C_MENU_BG), (6, C_MENU_BG),
                          (8, C_TASKBAR_BOT), (11, C_MENU_BG)):
            R(Rec(TMT_FILLCOLOR, T_COLOR, sp, part, 0, col))

    # ---- menus ----
    mn = cid_of("Menu")
    if mn is not None:
        for part, col in ((MP_MENUITEM, C_MENU_BG),
                          (MP_MENUDROPDOWN, C_MENU_BG),
                          (MP_MENUBARITEM, C_MENU_BG),
                          (MP_MENUBARDROPDOWN, C_MENU_BG),
                          (MP_CHEVRON, C_MENU_BG),
                          (MP_SEPARATOR, C_MENU_TXT)):
            R(Rec(TMT_FILLCOLOR, T_COLOR, mn, part, 0, col))
            R(Rec(TMT_TEXTCOLOR, T_COLOR, mn, part, 0, C_MENU_TXT))
        # highlighted states
        for part in (MP_MENUITEM, MP_MENUBARITEM):
            R(Rec(TMT_FILLCOLOR, T_COLOR, mn, part, 2, C_MENU_HOT))  # hot
            R(Rec(TMT_TEXTCOLOR, T_COLOR, mn, part, 2, C_TASKBAR_TXT))

    # ---- taskband ----
    tbd = cid_of("TaskBand")
    if tbd is not None:
        for part in (1, 2, 3):
            R(Rec(TMT_FILLCOLOR, T_COLOR, tbd, part, 0, C_TASKBAR_BOT))
    return recs
