#pragma once

#include "wiz8/local_code/ControlsRect.h"
#include "wiz8/canvas.h"
#include "wiz8/local_code/Widget.h"
#include "wiz8/regions.h"
#include "wiz8/vector.h"
#include "input.h"

/* The single-space separator wrapped notice lines are re-joined with. */
extern wchar_t g_W8TextSeparator[];

// VTABLE: WIZ8 0x005ed5b0
// class W8Vector<W8Widget*>

/* The region callback a widget without its own region is given. It answers
   whether the event was consumed; the screen-input dispatcher returns that
   byte to its caller. */
extern unsigned char DispatchControlRegionEvent(const InputAtom* event, struct W8Region* region);

/* The simple button-region policy shared by camp tabs, scrolling and
   movement controls. Repeat presses again; release is unconditional. The
   panel-owned dispatcher above has a different repeat/release policy. */
inline unsigned char DispatchButtonRegionEvent(const InputAtom* event, W8Region* region,
                                               W8Widget* button)
{
    switch (event->usEvent) {
    case LEFT_BUTTON_DOWN:
    case LEFT_BUTTON_REPEAT:
        button->OnLeftButtonDown(0);
        region->flags |= W8_REGION_LEFT_BUTTON_HELD;
        return 1;
    case LEFT_BUTTON_UP:
        button->OnLeftButtonUp(0);
        if ((region->flags & W8_REGION_LEFT_BUTTON_HELD) != 0) {
            region->flags &= ~W8_REGION_LEFT_BUTTON_HELD;
        }
        return 1;
    case MOUSE_POS:
        if ((region->flags & W8_REGION_MOUSE_LEAVE) != 0) {
            button->OnMouseLeave(0);
            return 1;
        }
        if ((region->flags & W8_REGION_MOUSE_ENTER) != 0) {
            button->OnMouseEnter(0);
            return 1;
        }
        break;
    }
    return 0;
}

/* Wide canvas, layout A (2026-10-12): how the background of a bottom-panel
   subscreen panel fills the wide bottom panel. Set by PlaceSubscreenPanel
   (wiz8/local_screens/MainGameLayout.h); all other panels have 0 and are
   drawn as in the original. Outside layout A (W = 640, camp) also as in the
   original. */
enum W8PanelWideMode {
    W8_PANEL_WIDE_NONE = 0,     /* one image at the panel origin */
    W8_PANEL_WIDE_STRETCH = 1,  /* background 2*ox wider: a slice inside the background is repeated */
    W8_PANEL_WIDE_FILL_GAPS = 2 /* panel centered, ox-wide gaps on both sides continue the background */
};

/* Panel/container, not a W8Widget base. Widgets register their pointers in
   m_controls and retain a back-pointer in m_pPanel. DestroyAllControls performs
   explicit child deletion; the vector itself only owns its pointer storage.
   The accumulated redraw rectangle uses left == -1 for an empty rectangle. */
// VTABLE: WIZ8 0x005ed5a4
struct Controls {
    Controls();
    Controls(int left, int top, int right, int bottom, int catalog_object, int catalog_frame,
             int catalog_image);
    ~Controls();

    virtual void SetEnabled(bool enable);
    virtual void Invalidate(const W8ControlsRect* rect);
    virtual void Redraw();
    /* SetEnabled mirrors m_fEnabled into every child's m_active. */
    bool m_fEnabled;
    bool m_fDirty;
    bool m_fLayoutDirty;
    /* Wide canvas layout A (2026-10-11, retail padding byte): the panel's
       W8LayoutAnchor. Its own art and children without an anchored region
       are drawn at that canvas edge (see SetLayoutAnchor). */
    unsigned char m_layoutAnchor;
    W8ControlsRect m_bounds; /* widget rectangles are relative to its origin */
    int m_catalogObject;     /* -1 skips catalog-image drawing */
    int m_catalogFrame;      /* frame within the catalog object */
    int m_catalogImage;      /* subimage within the frame */
    W8ControlsRect m_dirtyRect;
    bool m_fWholeAreaDirty; /* set when a caller passes no rectangle */
    /* Layout A (2026-10-12, original's alignment padding byte): W8PanelWideMode. */
    unsigned char m_wideMode;
    W8Vector<W8Widget*> m_controls;
    unsigned int m_uiRegionSetId;

    void EnableRegionSet(bool enable);
    void RemoveControl(W8Widget* control);
    void DestroyAllControls();
    void InvalidateLayout();
    void SetBounds(int left, int top, int right, int bottom);
    void AcquireRegionSet(unsigned int* shared_region_set);

    /* Wide canvas layout A: stick the panel and every child's mouse region to
       a canvas edge (W8LayoutAnchor). Coordinates stay 640-space. */
    void SetLayoutAnchor(int anchor)
    {
        m_layoutAnchor = static_cast<unsigned char>(anchor);
        for (int index = 0; index < m_controls.GetCount(); ++index) {
            if (ControlAt(index)->m_region != -1) {
                SetRegionLayoutAnchor(ControlAt(index)->m_region, anchor);
            }
        }
    }
    /* Where a child draws: its own region's anchor when that is set (the
       full-screen portrait panels mix both columns), else the panel's. */
    int ChildLayoutAnchor(W8Widget* child)
    {
        if (child->m_region != -1) {
            int anchor = GetRegionLayoutAnchor(child->m_region);
            if (anchor != W8_ANCHOR_CENTER) {
                return anchor;
            }
        }
        return m_layoutAnchor;
    }

    /* Draw the panel background (image from catalog image, frame m_catalogFrame) at
       the panel origin. In layout A with m_wideMode != 0 - stretched or with
       filled gaps (DrawSubscreenPanelArt). Call inside the panel's anchor
       scope (CanvasDrawScope(m_layoutAnchor)). */
    void DrawBackground(int image);

    /* The bounds-checked element read every walker above shares. Out of range
       it answers element zero rather than failing, which is what the canonical
       `p = m_ppControls; if (i < m_nControls) p += i;` compiles from and why
       the guard shows up once per use rather than once per loop. */
    inline W8Widget* ControlAt(int index)
    {
        return *m_controls.GetAt(index);
    }

protected:
    void RedrawChildren(bool full_redraw)
    {
        for (int index = 0; index < m_controls.GetCount(); ++index) {
            if (ControlAt(index)->m_active) {
                /* Layout A: each child at its own canvas edge. */
                CanvasDrawScope layout_scope(ChildLayoutAnchor(ControlAt(index)));
                ControlAt(index)->Redraw(full_redraw);
            }
        }
        m_fLayoutDirty = false;
    }
    void RedrawControls(bool full_redraw);
};

inline void DestroyControlPanel(Controls*& panel)
{
    if (panel != 0) {
        delete panel;
        panel = 0;
    }
}
