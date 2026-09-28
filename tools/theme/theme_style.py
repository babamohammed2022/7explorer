"""7explorer theme style v2: authored, Win7-Aero-like.

All values are original work (palette, gradients, metrics, fonts);
nothing was extracted from any Microsoft binary. The shape of the data
(record IDs, part/state integer IDs) follows the documented tmschema
interface schema, which is the interop contract of uxtheme.
"""
import struct

from build_theme import (Rec, T_COLOR, T_INT, T_BOOL, T_STRING,
                         T_MARGINS, T_ENUM, T_FONT, rgb,
                         TMT_FILLCOLOR, TMT_TEXTCOLOR, TMT_HEIGHT,
                         TMT_SIZINGMARGINS, TMT_CONTENTMARGINS,
                         TMT_FLATMENUS)

TMT_FILLTYPE = 4002          # enum: 1 = vertical gradient, 2 = horizontal
TMT_GRADIENTCOLOR1 = 3811
TMT_GRADIENTCOLOR2 = 3812
TMT_TRANSPARENTCOLOR = 3801
TMT_BORDERSIZE = 2403

FS_VERTGRAD, FS_HORZGRAD = 1, 2

CLS7 = ["Taskbar", "TaskBand", "Rebar", "StartPanel",
        "TrayNotify", "Clock", "Menu", "MenuBand", "StartMenu",
        "TaskBar::Taskbar", "Toolbar", "Tooltip"]

# ---------------- authored win7-like palette (original values) -----------
# glass taskbar: deep desaturated blue, darker at the bottom edge
P = {
    "tb_top":     rgb(0x5A, 0x74, 0x8C),
    "tb_bottom":  rgb(0x1D, 0x2A, 0x38),
    "tb_fill":    rgb(0x2F, 0x3F, 0x4F),
    "tb_txt":     rgb(0xF2, 0xF6, 0xFA),
    "rebar":      rgb(0x24, 0x33, 0x41),
    "grip":       rgb(0x1A, 0x27, 0x33),
    "btn":        rgb(0x3A, 0x4E, 0x63),
    "btn_hot":    rgb(0x6E, 0x8A, 0xA6),
    "btn_press":  rgb(0x22, 0x32, 0x43),
    "btn_check":  rgb(0x2A, 0x42, 0x5B),
    "btn_flash":  rgb(0xB9, 0x85, 0x2E),
    "menu_bg":    rgb(0xF4, 0xF5, 0xF6),
    "menu_hot":   rgb(0xC8, 0xE0, 0xF8),
    "menu_hot2":  rgb(0xAE, 0xD0, 0xEE),
    "menu_txt":   rgb(0x1F, 0x1F, 0x1F),
    "menu_sep":   rgb(0xDA, 0xDA, 0xDA),
    "sp_left":    rgb(0xFF, 0xFF, 0xFF),
    "sp_right":   rgb(0xDF, 0xEA, 0xF6),
    "sp_top":     rgb(0x45, 0x6E, 0x91),
    "sp_bottom":  rgb(0x24, 0x32, 0x42),
    "sp_title":   rgb(0xF2, 0xF6, 0xFA),
    "sp_arrow":   rgb(0xF0, 0xA9, 0x3A),
    "tt_bg":      rgb(0xFC, 0xFB, 0xE9),
    "tt_txt":     rgb(0x1F, 0x1F, 0x1F),
    "tray":       rgb(0x21, 0x30, 0x3F),
}

TBP_BG = (1, 2, 3, 4)
TBP_SIZING = (5, 6, 7, 8)
TBP_SIZINGBARB = 5
RP_GRIPPER, RP_GRIPPERVERT, RP_BAND, RP_CHEVRON, RP_CHEVRONVERT = 1, 2, 3, 4, 5
TNP_BACKGROUND, TNP_ANIMBACKGROUND = 1, 2
CLP_TIME = 1
MP_MENUITEM, MP_MENUDROPDOWN, MP_MENUBARITEM, MP_MENUBARDROPDOWN = 1, 2, 3, 4
MP_CHEVRON, MP_SEPARATOR = 5, 6
MS_NORMAL, MS_SELECTED, MS_DEMOTED = 1, 2, 3
TS_NORMAL, TS_HOT, TS_PRESSED, TS_DISABLED, TS_CHECKED, TS_HOTCHECKED = 1, 2, 3, 4, 5, 6
SPP_USERPANE, SPP_MOREPROGRAMS, SPP_MOREPROGRAMSARROW, SPP_PROGLIST = 1, 2, 3, 4
SPP_PROGLISTSEPARATOR, SPP_PLACESLIST, SPP_PLACESLISTSEPARATOR = 5, 6, 7
SPP_LOGOFF, SPP_LOGOFFBUTTONS, SPP_USERPICTURE, SPP_PREVIEW = 8, 9, 10, 11
TP_BUTTON, TP_DROPDOWNBUTTON, TP_SPLITBUTTON, TP_SPLITBUTTONDROPDOWN = 1, 2, 3, 4
TTP_STANDARD, TTP_BALLOON = 1, 3
TDP_GROUPCOUNT, TDP_FLASHBUTTON, TDP_FLASHBUTTONGROUPMENU = 1, 2, 3


def _logfont(face, height, weight=400, italic=0):
    """LOGFONTW, 92 bytes (authored face/size)."""
    LF_FACESIZE = 32  # wchars
    name = face.encode("utf-16-le")[:62]
    name = name + b"\0" * ((LF_FACESIZE - 1 - len(face)) * 2 + 2)
    head = struct.pack("<iiiii", height, 0, 0, 0, weight)
    flags = struct.pack("<BBBBBBBB", italic, 0, 0, 1, 3, 2, 6, 68)
    return head + flags + name


# font ids used by the size-variant classes (interface constants)
FONT_IDS = (501, 502, 503, 504, 505, 506)


def _font_records_for(cid, R):
    """Segoe UI 9pt family, authored values (Win7 shell look)."""
    base = _logfont("Segoe UI", -12)
    bold = _logfont("Segoe UI", -12, 700)
    fonts = (bold, _logfont("Segoe UI", -11), base, base, base,
             _logfont("Segoe UI", -11))
    for i, fid in enumerate(FONT_IDS):
        R(Rec(801 + i, T_FONT, cid, 0, 0, (fid, fonts[i])))


def make_style_records(cid_of):
    recs = []
    R = recs.append

    # ---------- reserved variant classes (required to exist) -------------
    for nm in ("sizevariant.NormalSize", "sizevariant.Default",
               "colorvariant.NormalColor"):
        cid = cid_of(nm)
        if cid is not None:
            _font_records_for(cid, R)
            R(Rec(TMT_HEIGHT, T_INT, cid, 0, 0, 16))   # border banding
    for nm in ("globals", "sysmetrics"):
        cid = cid_of(nm)
        if cid is not None:
            R(Rec(TMT_HEIGHT, T_INT, cid, 0, 0, 16))

    cid = cid_of("globals")
    if cid is not None:
        R(Rec(TMT_FLATMENUS, T_BOOL, cid, 0, 0, 1))

    # ---------- taskbar: glassy vertical gradient -------------------------
    tb = cid_of("Taskbar")
    if tb is not None:
        for part in TBP_BG + TBP_SIZING:
            R(Rec(TMT_FILLTYPE, T_ENUM, tb, part, 0, FS_VERTGRAD))
            R(Rec(TMT_GRADIENTCOLOR1, T_COLOR, tb, part, 0, P["tb_top"]))
            R(Rec(TMT_GRADIENTCOLOR2, T_COLOR, tb, part, 0, P["tb_bottom"]))
            R(Rec(TMT_FILLCOLOR, T_COLOR, tb, part, 0, P["tb_fill"]))
            R(Rec(TMT_TEXTCOLOR, T_COLOR, tb, part, 0, P["tb_txt"]))
        for part in TBP_BG:
            R(Rec(TMT_HEIGHT, T_INT, tb, part, 0, 40))
            R(Rec(TMT_SIZINGMARGINS, T_MARGINS, tb, part, 0, (4, 4, 4, 4)))
            R(Rec(TMT_CONTENTMARGINS, T_MARGINS, tb, part, 0, (2, 2, 2, 2)))
        for part in TBP_SIZING:
            R(Rec(TMT_HEIGHT, T_INT, tb, part, 0, 3))

    # ---------- taskband flash hint --------------------------------------
    tbd = cid_of("TaskBand")
    if tbd is not None:
        R(Rec(TMT_FILLCOLOR, T_COLOR, tbd, TDP_FLASHBUTTON, 0,
              P["btn_flash"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, tbd, TDP_FLASHBUTTONGROUPMENU, 0,
              P["btn_flash"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, tbd, TDP_GROUPCOUNT, 0, P["btn"]))

    # ---------- rebar / mini-toolbars on taskbar --------------------------
    rb = cid_of("Rebar")
    if rb is not None:
        R(Rec(TMT_FILLTYPE, T_ENUM, rb, RP_BAND, 0, FS_VERTGRAD))
        R(Rec(TMT_GRADIENTCOLOR1, T_COLOR, rb, RP_BAND, 0, P["tb_top"]))
        R(Rec(TMT_GRADIENTCOLOR2, T_COLOR, rb, RP_BAND, 0, P["tb_bottom"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, rb, RP_BAND, 0, P["rebar"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, rb, RP_GRIPPER, 0, P["grip"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, rb, RP_GRIPPERVERT, 0, P["grip"]))
        for part in (RP_CHEVRON, RP_CHEVRONVERT):
            R(Rec(TMT_FILLCOLOR, T_COLOR, rb, part, TS_NORMAL, P["btn"]))
            R(Rec(TMT_FILLCOLOR, T_COLOR, rb, part, TS_HOT, P["btn_hot"]))
            R(Rec(TMT_FILLCOLOR, T_COLOR, rb, part, TS_PRESSED,
                  P["btn_press"]))

    # ---------- tray & clock ---------------------------------------------
    tn = cid_of("TrayNotify")
    if tn is not None:
        for part in (TNP_BACKGROUND, TNP_ANIMBACKGROUND):
            R(Rec(TMT_FILLTYPE, T_ENUM, tn, part, 0, FS_VERTGRAD))
            R(Rec(TMT_GRADIENTCOLOR1, T_COLOR, tn, part, 0, P["tb_top"]))
            R(Rec(TMT_GRADIENTCOLOR2, T_COLOR, tn, part, 0, P["tb_bottom"]))
            R(Rec(TMT_FILLCOLOR, T_COLOR, tn, part, 0, P["tray"]))
    ck = cid_of("Clock")
    if ck is not None:
        R(Rec(TMT_TEXTCOLOR, T_COLOR, ck, CLP_TIME, 0, P["tb_txt"]))

    # ---------- start panel ------------------------------------------------
    sp = cid_of("StartPanel")
    if sp is not None:
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_USERPANE, 0, P["sp_top"]))
        R(Rec(TMT_TEXTCOLOR, T_COLOR, sp, SPP_USERPANE, 0, P["sp_title"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_PROGLIST, 0, P["sp_left"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_PREVIEW, 0, P["sp_left"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_PLACESLIST, 0, P["sp_right"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_LOGOFF, 0, P["sp_bottom"]))
        R(Rec(TMT_TEXTCOLOR, T_COLOR, sp, SPP_LOGOFFBUTTONS, 0,
              P["sp_title"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_LOGOFFBUTTONS, TS_HOT,
              P["btn_hot"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_MOREPROGRAMS, 0, P["sp_left"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_MOREPROGRAMSARROW, 0,
              P["sp_arrow"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_PROGLISTSEPARATOR, 0,
              P["menu_sep"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, sp, SPP_PLACESLISTSEPARATOR, 0,
              P["menu_sep"]))
        R(Rec(TMT_CONTENTMARGINS, T_MARGINS, sp, SPP_PROGLIST, 0,
              (4, 4, 4, 4)))

    # ---------- menus: light popup with blue selection ----------------------
    mn = cid_of("Menu")
    if mn is not None:
        for part in (MP_MENUITEM, MP_MENUDROPDOWN, MP_MENUBARITEM,
                     MP_MENUBARDROPDOWN, MP_CHEVRON):
            R(Rec(TMT_FILLCOLOR, T_COLOR, mn, part, MS_NORMAL, P["menu_bg"]))
            R(Rec(TMT_TEXTCOLOR, T_COLOR, mn, part, MS_NORMAL, P["menu_txt"]))
            R(Rec(TMT_FILLTYPE, T_ENUM, mn, part, MS_SELECTED,
                  FS_VERTGRAD))
            R(Rec(TMT_GRADIENTCOLOR1, T_COLOR, mn, part, MS_SELECTED,
                  P["menu_hot"]))
            R(Rec(TMT_GRADIENTCOLOR2, T_COLOR, mn, part, MS_SELECTED,
                  P["menu_hot2"]))
            R(Rec(TMT_FILLCOLOR, T_COLOR, mn, part, MS_SELECTED,
                  P["menu_hot"]))
            R(Rec(TMT_TEXTCOLOR, T_COLOR, mn, part, MS_SELECTED,
                  P["menu_txt"]))
            R(Rec(TMT_FILLCOLOR, T_COLOR, mn, part, MS_DEMOTED,
                  P["menu_bg"]))
            R(Rec(TMT_TEXTCOLOR, T_COLOR, mn, part, MS_DEMOTED,
                  P["menu_txt"]))
        for part in (MP_SEPARATOR,):
            R(Rec(TMT_FILLCOLOR, T_COLOR, mn, part, MS_NORMAL,
                  P["menu_sep"]))
            R(Rec(TMT_HEIGHT, T_INT, mn, part, MS_NORMAL, 1))

    # ---------- menuband ----------------------------------------------------
    mb = cid_of("MenuBand")
    if mb is not None:
        R(Rec(TMT_FILLCOLOR, T_COLOR, mb, 1, 0, P["menu_bg"]))
        R(Rec(TMT_TEXTCOLOR, T_COLOR, mb, 1, 0, P["menu_txt"]))
        R(Rec(TMT_FILLCOLOR, T_COLOR, mb, 2, 0, P["menu_sep"]))

    # ---------- toolbar ------------------------------------------------------
    tl = cid_of("Toolbar")
    if tl is not None:
        for part in (TP_BUTTON, TP_DROPDOWNBUTTON, TP_SPLITBUTTON,
                     TP_SPLITBUTTONDROPDOWN):
            for st, col in ((TS_NORMAL, P["btn"]),
                            (TS_HOT, P["btn_hot"]),
                            (TS_HOTCHECKED, P["btn_hot"]),
                            (TS_CHECKED, P["btn_check"]),
                            (TS_PRESSED, P["btn_press"]),
                            (TS_DISABLED, P["btn"])):
                R(Rec(TMT_FILLCOLOR, T_COLOR, tl, part, st, col))
            R(Rec(TMT_TEXTCOLOR, T_COLOR, tl, part, TS_NORMAL, P["tb_txt"]))

    # ---------- tooltip -------------------------------------------------------
    tp = cid_of("Tooltip")
    if tp is not None:
        for part in (TTP_STANDARD, TTP_BALLOON):
            R(Rec(TMT_FILLCOLOR, T_COLOR, tp, part, 0, P["tt_bg"]))
            R(Rec(TMT_TEXTCOLOR, T_COLOR, tp, part, 0, P["tt_txt"]))
            R(Rec(TMT_CONTENTMARGINS, T_MARGINS, tp, part, 0, (8, 4, 8, 4)))
    return recs
