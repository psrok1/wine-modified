/*
 * Window position related functions.
 *
 * Copyright 1993, 1994, 1995 Alexandre Julliard
 *                       1995,1996 Alex Korobka
 */

#include "sysmetrics.h"
#include "module.h"
#include "user.h"
#include "win.h"
#include "event.h"
#include "hook.h"
#include "message.h"
#include "queue.h"
#include "stackframe.h"
#include "winpos.h"
#include "nonclient.h"
#include "stddebug.h"
/* #define DEBUG_WIN */
#include "debug.h"

#define  SWP_NOPOSCHANGE	(SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE)

/* ----- external functions ----- */

void 	FOCUS_SwitchFocus( HWND , HWND );
HRGN 	DCE_GetVisRgn( HWND, WORD );

/* ----- internal variables ----- */

static HWND hwndActive      = 0;  /* Currently active window */
static HWND hwndPrevActive  = 0;  /* Previously active window */

/***********************************************************************
 *           WINPOS_FindIconPos
 *
 * Find a suitable place for an iconic window.
 * The new position is stored into wndPtr->ptIconPos.
 */
void WINPOS_FindIconPos( HWND hwnd )
{
    RECT rectParent;
    short x, y, xspacing, yspacing;
    WND * wndPtr = WIN_FindWndPtr( hwnd );

    if (!wndPtr || !wndPtr->parent) return;
    GetClientRect( wndPtr->parent->hwndSelf, &rectParent );
    if ((wndPtr->ptIconPos.x >= rectParent.left) &&
        (wndPtr->ptIconPos.x + SYSMETRICS_CXICON < rectParent.right) &&
        (wndPtr->ptIconPos.y >= rectParent.top) &&
        (wndPtr->ptIconPos.y + SYSMETRICS_CYICON < rectParent.bottom))
        return;  /* The icon already has a suitable position */

    xspacing = yspacing = 70;  /* FIXME: This should come from WIN.INI */
    y = rectParent.bottom;
    for (;;)
    {
        for (x = rectParent.left; x<=rectParent.right-xspacing; x += xspacing)
        {
              /* Check if another icon already occupies this spot */
            WND *childPtr = wndPtr->parent->child;
            while (childPtr)
            {
                if ((childPtr->dwStyle & WS_MINIMIZE) && (childPtr != wndPtr))
                {
                    if ((childPtr->rectWindow.left < x + xspacing) &&
                        (childPtr->rectWindow.right >= x) &&
                        (childPtr->rectWindow.top <= y) &&
                        (childPtr->rectWindow.bottom > y - yspacing))
                        break;  /* There's a window in there */
                }
                childPtr = childPtr->next;
            }
            if (!childPtr)
            {
                  /* No window was found, so it's OK for us */
                wndPtr->ptIconPos.x = x + (xspacing - SYSMETRICS_CXICON) / 2;
                wndPtr->ptIconPos.y = y - (yspacing + SYSMETRICS_CYICON) / 2;
                return;
            }
        }
        y -= yspacing;
    }
}


/***********************************************************************
 *           ArrangeIconicWindows   (USER.170)
 */
UINT ArrangeIconicWindows( HWND parent )
{
    RECT rectParent;
    HWND hwndChild;
    INT x, y, xspacing, yspacing;

    GetClientRect( parent, &rectParent );
    x = rectParent.left;
    y = rectParent.bottom;
    xspacing = yspacing = 70;  /* FIXME: This should come from WIN.INI */
    hwndChild = GetWindow( parent, GW_CHILD );
    while (hwndChild)
    {
        if (IsIconic( hwndChild ))
        {
            SetWindowPos( hwndChild, 0, x + (xspacing - SYSMETRICS_CXICON) / 2,
                          y - (yspacing + SYSMETRICS_CYICON) / 2, 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
            if (x <= rectParent.right - xspacing) x += xspacing;
            else
            {
                x = rectParent.left;
                y -= yspacing;
            }
        }
        hwndChild = GetWindow( hwndChild, GW_HWNDNEXT );
    }
    return yspacing;
}


/***********************************************************************
 *           GetWindowRect   (USER.32)
 */
void GetWindowRect( HWND hwnd, LPRECT rect ) 
{
    WND * wndPtr = WIN_FindWndPtr( hwnd ); 
    if (!wndPtr) return;
    
    *rect = wndPtr->rectWindow;
    if (wndPtr->dwStyle & WS_CHILD)
	MapWindowPoints( wndPtr->parent->hwndSelf, 0, (POINT *)rect, 2 );
}


/***********************************************************************
 *           GetClientRect   (USER.33)
 */
void GetClientRect( HWND hwnd, LPRECT rect ) 
{
    WND * wndPtr = WIN_FindWndPtr( hwnd );

    rect->left = rect->top = rect->right = rect->bottom = 0;
    if (wndPtr) 
    {
	rect->right  = wndPtr->rectClient.right - wndPtr->rectClient.left;
	rect->bottom = wndPtr->rectClient.bottom - wndPtr->rectClient.top;
    }
}


/*******************************************************************
 *         ClientToScreen   (USER.28)
 */
BOOL ClientToScreen( HWND hwnd, LPPOINT lppnt )
{
    MapWindowPoints( hwnd, 0, lppnt, 1 );
    return TRUE;
}


/*******************************************************************
 *         ScreenToClient   (USER.29)
 */
void ScreenToClient( HWND hwnd, LPPOINT lppnt )
{
    MapWindowPoints( 0, hwnd, lppnt, 1 );
}


/***********************************************************************
 *           WINPOS_WindowFromPoint
 *
 * Find the window and hittest for a given point.
 */
INT WINPOS_WindowFromPoint( POINT pt, WND **ppWnd )
{
    WND *wndPtr;
    INT hittest = HTERROR;
    INT x, y;

    *ppWnd = NULL;
    x = pt.x;
    y = pt.y;
    wndPtr = WIN_GetDesktop()->child;
    for (;;)
    {
        while (wndPtr)
        {
            /* If point is in window, and window is visible, and it  */
            /* is enabled (or it's a top-level window), then explore */
            /* its children. Otherwise, go to the next window.       */

            if ((wndPtr->dwStyle & WS_VISIBLE) &&
                (!(wndPtr->dwStyle & WS_DISABLED) ||
                 ((wndPtr->dwStyle & (WS_POPUP | WS_CHILD)) != WS_CHILD)) &&
                (x >= wndPtr->rectWindow.left) &&
                (x < wndPtr->rectWindow.right) &&
                (y >= wndPtr->rectWindow.top) &&
                (y < wndPtr->rectWindow.bottom))
            {
                *ppWnd = wndPtr;  /* Got a suitable window */

                /* If window is minimized or disabled, return at once */
                if (wndPtr->dwStyle & WS_MINIMIZE) return HTCAPTION;
                if (wndPtr->dwStyle & WS_DISABLED) return HTERROR;

                /* If point is not in client area, ignore the children */
                if ((x < wndPtr->rectClient.left) ||
                    (x >= wndPtr->rectClient.right) ||
                    (y < wndPtr->rectClient.top) ||
                    (y >= wndPtr->rectClient.bottom)) break;

                x -= wndPtr->rectClient.left;
                y -= wndPtr->rectClient.top;
                wndPtr = wndPtr->child;
            }
            else wndPtr = wndPtr->next;
        }

        /* If nothing found, return the desktop window */
        if (!*ppWnd)
        {
            *ppWnd = WIN_GetDesktop();
            return HTCLIENT;
        }

        /* Send the WM_NCHITTEST message (only if to the same task) */
        if ((*ppWnd)->hmemTaskQ != GetTaskQueue(0)) return HTCLIENT;
        hittest = (INT)SendMessage( (*ppWnd)->hwndSelf, WM_NCHITTEST, 0,
                                    MAKELONG( pt.x, pt.y ) );
        if (hittest != HTTRANSPARENT) return hittest;  /* Found the window */

        /* If no children found in last search, make point relative to parent*/
        if (!wndPtr)
        {
            x += (*ppWnd)->rectClient.left;
            y += (*ppWnd)->rectClient.top;
        }

        /* Restart the search from the next sibling */
        wndPtr = (*ppWnd)->next;
        *ppWnd = (*ppWnd)->parent;
    }
}


/*******************************************************************
 *         WindowFromPoint   (USER.30)
 */
HWND WindowFromPoint( POINT pt )
{
    WND *pWnd;
    WINPOS_WindowFromPoint( pt, &pWnd );
    return pWnd->hwndSelf;
}


/*******************************************************************
 *         ChildWindowFromPoint   (USER.191)
 */
HWND ChildWindowFromPoint( HWND hwndParent, POINT pt )
{
    /* pt is in the client coordinates */

    WND* wnd = WIN_FindWndPtr(hwndParent);
    RECT rect;

    if( !wnd ) return 0;

    /* get client rect fast */
    rect.top = rect.left = 0;
    rect.right = wnd->rectClient.right - wnd->rectClient.left;
    rect.bottom = wnd->rectClient.bottom - wnd->rectClient.top;

    if (!PtInRect( &rect, pt )) return 0;

    wnd = wnd->child;
    while ( wnd )
    {
        if (PtInRect( &wnd->rectWindow, pt )) return wnd->hwndSelf;
        wnd = wnd->next;
    }
    return hwndParent;
}

/*******************************************************************
 *         MapWindowPoints   (USER.258)
 */
void MapWindowPoints( HWND hwndFrom, HWND hwndTo, LPPOINT lppt, WORD count )
{
    WND * wndPtr;
    POINT * curpt;
    POINT origin = { 0, 0 };
    WORD i;

    if( hwndFrom == hwndTo ) return;

      /* Translate source window origin to screen coords */
    if (hwndFrom)
    {
        if (!(wndPtr = WIN_FindWndPtr( hwndFrom )))
        {
            fprintf(stderr,"MapWindowPoints: bad hwndFrom = %04x\n",hwndFrom);
            return;
        }
        while (wndPtr->parent)
        {
            origin.x += wndPtr->rectClient.left;
            origin.y += wndPtr->rectClient.top;
            wndPtr = wndPtr->parent;
        }
    }

      /* Translate origin to destination window coords */
    if (hwndTo)
    {
        if (!(wndPtr = WIN_FindWndPtr( hwndTo )))
        {
            fprintf(stderr,"MapWindowPoints: bad hwndTo = %04x\n", hwndTo );
            return;
        }
        while (wndPtr->parent)
        {
            origin.x -= wndPtr->rectClient.left;
            origin.y -= wndPtr->rectClient.top;
            wndPtr = wndPtr->parent;
        }    
    }

      /* Translate points */
    for (i = 0, curpt = lppt; i < count; i++, curpt++)
    {
	curpt->x += origin.x;
	curpt->y += origin.y;
    }
}


/***********************************************************************
 *           IsIconic   (USER.31)
 */
BOOL IsIconic(HWND hWnd)
{
    WND * wndPtr = WIN_FindWndPtr(hWnd);
    if (wndPtr == NULL) return FALSE;
    return (wndPtr->dwStyle & WS_MINIMIZE) != 0;
}
 
 
/***********************************************************************
 *           IsZoomed   (USER.272)
 */
BOOL IsZoomed(HWND hWnd)
{
    WND * wndPtr = WIN_FindWndPtr(hWnd);
    if (wndPtr == NULL) return FALSE;
    return (wndPtr->dwStyle & WS_MAXIMIZE) != 0;
}


/*******************************************************************
 *         GetActiveWindow    (USER.60)
 */
HWND GetActiveWindow()
{
    return hwndActive;
}


/*******************************************************************
 *         SetActiveWindow    (USER.59)
 */
HWND SetActiveWindow( HWND hwnd )
{
    HWND prev = hwndActive;
    WND *wndPtr = WIN_FindWndPtr( hwnd );

    if (!wndPtr || (wndPtr->dwStyle & WS_DISABLED) ||
	!(wndPtr->dwStyle & WS_VISIBLE)) return 0;

    WINPOS_SetActiveWindow( hwnd, 0, 0 );
    return prev;
}


/***********************************************************************
 *           BringWindowToTop   (USER.45)
 */
BOOL BringWindowToTop( HWND hwnd )
{
    return SetWindowPos( hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE );
}


/***********************************************************************
 *           MoveWindow   (USER.56)
 */
BOOL MoveWindow( HWND hwnd, short x, short y, short cx, short cy, BOOL repaint)
{    
    int flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (!repaint) flags |= SWP_NOREDRAW;
    dprintf_win(stddeb, "MoveWindow: %04x %d,%d %dx%d %d\n", 
	    hwnd, x, y, cx, cy, repaint );
    return SetWindowPos( hwnd, 0, x, y, cx, cy, flags );
}


/***********************************************************************
 *           ShowWindow   (USER.42)
 */
BOOL ShowWindow( HWND hwnd, int cmd ) 
{    
    WND * wndPtr = WIN_FindWndPtr( hwnd );
    BOOL wasVisible;
    POINT maxSize;
    int swpflags = 0;
    short x = 0, y = 0, cx = 0, cy = 0;

    if (!wndPtr) return FALSE;

    dprintf_win(stddeb,"ShowWindow: hwnd=%04x, cmd=%d\n", hwnd, cmd);

    wasVisible = (wndPtr->dwStyle & WS_VISIBLE) != 0;

    switch(cmd)
    {
        case SW_HIDE:
            if (!wasVisible) return FALSE;
	    swpflags |= SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE | 
		        SWP_NOACTIVATE | SWP_NOZORDER;
	    break;

	case SW_SHOWMINNOACTIVE:
            swpflags |= SWP_NOACTIVATE | SWP_NOZORDER;
            /* fall through */
	case SW_SHOWMINIMIZED:
            swpflags |= SWP_SHOWWINDOW;
            /* fall through */
	case SW_MINIMIZE:
            swpflags |= SWP_FRAMECHANGED;
            if (!(wndPtr->dwStyle & WS_MINIMIZE))
            {
                if (wndPtr->dwStyle & WS_MAXIMIZE)
                {
                    wndPtr->flags |= WIN_RESTORE_MAX;
                    wndPtr->dwStyle &= ~WS_MAXIMIZE;
                }
                else
                {
                    wndPtr->flags &= ~WIN_RESTORE_MAX;
                    wndPtr->rectNormal = wndPtr->rectWindow;
                }
                wndPtr->dwStyle |= WS_MINIMIZE;
                WINPOS_FindIconPos( hwnd );
                x  = wndPtr->ptIconPos.x;
                y  = wndPtr->ptIconPos.y;
                cx = SYSMETRICS_CXICON;
                cy = SYSMETRICS_CYICON;
            }
            else swpflags |= SWP_NOSIZE | SWP_NOMOVE;
	    break;

	case SW_SHOWMAXIMIZED: /* same as SW_MAXIMIZE: */
            swpflags |= SWP_SHOWWINDOW | SWP_FRAMECHANGED;
            if (!(wndPtr->dwStyle & WS_MAXIMIZE))
            {
                  /* Store the current position and find the maximized size */
                if (!(wndPtr->dwStyle & WS_MINIMIZE))
                    wndPtr->rectNormal = wndPtr->rectWindow; 

                NC_GetMinMaxInfo( hwnd, &maxSize,
                                  &wndPtr->ptMaxPos, NULL, NULL );
                x  = wndPtr->ptMaxPos.x;
                y  = wndPtr->ptMaxPos.y;

		if( wndPtr->dwStyle & WS_MINIMIZE )
		    if( !SendMessage( hwnd, WM_QUERYOPEN, 0, 0L ) )
			{
		         swpflags |= SWP_NOSIZE;
			 break;
			}

                cx = maxSize.x;
                cy = maxSize.y;
                wndPtr->dwStyle &= ~WS_MINIMIZE;
                wndPtr->dwStyle |= WS_MAXIMIZE;
            }
            else swpflags |= SWP_NOSIZE | SWP_NOMOVE;
            break;

	case SW_SHOWNA:
            swpflags |= SWP_NOACTIVATE | SWP_NOZORDER;
            /* fall through */
	case SW_SHOW:
	    swpflags |= SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;
	    break;

	case SW_SHOWNOACTIVATE:
            swpflags |= SWP_NOZORDER;
            if (GetActiveWindow()) swpflags |= SWP_NOACTIVATE;
            /* fall through */
	case SW_SHOWNORMAL:  /* same as SW_NORMAL: */
	case SW_SHOWDEFAULT: /* FIXME: should have its own handler */
	case SW_RESTORE:
	    swpflags |= SWP_SHOWWINDOW | SWP_FRAMECHANGED;

            if (wndPtr->dwStyle & WS_MINIMIZE)
            {
                if( !SendMessage( hwnd, WM_QUERYOPEN, 0, 0L) )
                  {
                    swpflags |= SWP_NOSIZE;
                    break;
                  }
                wndPtr->ptIconPos.x = wndPtr->rectWindow.left;
                wndPtr->ptIconPos.y = wndPtr->rectWindow.top;
                wndPtr->dwStyle &= ~WS_MINIMIZE;
                if (wndPtr->flags & WIN_RESTORE_MAX)
                {
                    /* Restore to maximized position */
                    NC_GetMinMaxInfo( hwnd, &maxSize, &wndPtr->ptMaxPos,
                                      NULL, NULL );
                    x  = wndPtr->ptMaxPos.x;
                    y  = wndPtr->ptMaxPos.y;
                    cx = maxSize.x;
                    cy = maxSize.y;
                   wndPtr->dwStyle |= WS_MAXIMIZE;
                }
                else  /* Restore to normal position */
                {
                    x  = wndPtr->rectNormal.left;
                    y  = wndPtr->rectNormal.top;
                    cx = wndPtr->rectNormal.right - wndPtr->rectNormal.left;
                    cy = wndPtr->rectNormal.bottom - wndPtr->rectNormal.top;
                }
            }
            else if (wndPtr->dwStyle & WS_MAXIMIZE)
            {
                wndPtr->ptMaxPos.x = wndPtr->rectWindow.left;
                wndPtr->ptMaxPos.y = wndPtr->rectWindow.top;
                wndPtr->dwStyle &= ~WS_MAXIMIZE;
                x  = wndPtr->rectNormal.left;
                y  = wndPtr->rectNormal.top;
                cx = wndPtr->rectNormal.right - wndPtr->rectNormal.left;
                cy = wndPtr->rectNormal.bottom - wndPtr->rectNormal.top;
            }
            else swpflags |= SWP_NOSIZE | SWP_NOMOVE;
	    break;
    }

    SendMessage( hwnd, WM_SHOWWINDOW, (cmd != SW_HIDE), 0 );
    SetWindowPos( hwnd, HWND_TOP, x, y, cx, cy, swpflags );

      /* Send WM_SIZE and WM_MOVE messages if not already done */
    if (!(wndPtr->flags & WIN_GOT_SIZEMSG))
    {
	int wParam = SIZE_RESTORED;
	if (wndPtr->dwStyle & WS_MAXIMIZE) wParam = SIZE_MAXIMIZED;
	else if (wndPtr->dwStyle & WS_MINIMIZE) wParam = SIZE_MINIMIZED;
	wndPtr->flags |= WIN_GOT_SIZEMSG;
	SendMessage( hwnd, WM_SIZE, wParam,
		     MAKELONG(wndPtr->rectClient.right-wndPtr->rectClient.left,
			    wndPtr->rectClient.bottom-wndPtr->rectClient.top));
	SendMessage( hwnd, WM_MOVE, 0,
		   MAKELONG(wndPtr->rectClient.left, wndPtr->rectClient.top) );
    }

    return wasVisible;
}


/***********************************************************************
 *           GetInternalWindowPos   (USER.460)
 */
WORD GetInternalWindowPos( HWND hwnd, LPRECT rectWnd, LPPOINT ptIcon )
{
    WINDOWPLACEMENT wndpl;
    if (!GetWindowPlacement( hwnd, &wndpl )) return 0;
    if (rectWnd) *rectWnd = wndpl.rcNormalPosition;
    if (ptIcon)  *ptIcon = wndpl.ptMinPosition;
    return wndpl.showCmd;
}


/***********************************************************************
 *           SetInternalWindowPos   (USER.461)
 */
void SetInternalWindowPos( HWND hwnd, WORD showCmd, LPRECT rect, LPPOINT pt )
{
    WINDOWPLACEMENT wndpl;
    WND *wndPtr = WIN_FindWndPtr( hwnd );

    wndpl.length  = sizeof(wndpl);
    wndpl.flags   = (pt != NULL) ? WPF_SETMINPOSITION : 0;
    wndpl.showCmd = showCmd;
    if (pt) wndpl.ptMinPosition = *pt;
    wndpl.rcNormalPosition = (rect != NULL) ? *rect : wndPtr->rectNormal;
    wndpl.ptMaxPosition = wndPtr->ptMaxPos;
    SetWindowPlacement( hwnd, &wndpl );
}


/***********************************************************************
 *           GetWindowPlacement   (USER.370)
 */
BOOL GetWindowPlacement( HWND hwnd, WINDOWPLACEMENT *wndpl )
{
    WND *wndPtr = WIN_FindWndPtr( hwnd );
    if (!wndPtr) return FALSE;

    wndpl->length  = sizeof(*wndpl);
    wndpl->flags   = 0;
    wndpl->showCmd = IsZoomed(hwnd) ? SW_SHOWMAXIMIZED : 
	             (IsIconic(hwnd) ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
    wndpl->ptMinPosition = wndPtr->ptIconPos;
    wndpl->ptMaxPosition = wndPtr->ptMaxPos;
    wndpl->rcNormalPosition = wndPtr->rectNormal;
    return TRUE;
}


/***********************************************************************
 *           SetWindowPlacement   (USER.371)
 */
BOOL SetWindowPlacement( HWND hwnd, WINDOWPLACEMENT *wndpl )
{
    WND *wndPtr = WIN_FindWndPtr( hwnd );
    if (!wndPtr) return FALSE;

    if (wndpl->flags & WPF_SETMINPOSITION)
	wndPtr->ptIconPos = wndpl->ptMinPosition;
    if ((wndpl->flags & WPF_RESTORETOMAXIMIZED) &&
	(wndpl->showCmd == SW_SHOWMINIMIZED)) wndPtr->flags |= WIN_RESTORE_MAX;
    wndPtr->ptMaxPos   = wndpl->ptMaxPosition;
    wndPtr->rectNormal = wndpl->rcNormalPosition;
    ShowWindow( hwnd, wndpl->showCmd );
    return TRUE;
}

/*******************************************************************
 *	   ACTIVATEAPP_callback
 */
BOOL ACTIVATEAPP_callback(HWND hWnd, LPARAM lParam)
{
    ACTIVATESTRUCT  *lpActStruct = (ACTIVATESTRUCT*)lParam;
 
    if (GetWindowTask(hWnd) != lpActStruct->hTaskSendTo) return 1;

    SendMessage( hWnd, WM_ACTIVATEAPP, lpActStruct->wFlag,
		(LPARAM)((lpActStruct->hWindowTask)?lpActStruct->hWindowTask:0));
    return 1;
}


/*******************************************************************
 *	   WINPOS_SetActiveWindow
 *
 * back-end to SetActiveWindow
 */
BOOL WINPOS_SetActiveWindow( HWND hWnd, BOOL fMouse, BOOL fChangeFocus )
{
    WND                   *wndPtr          = WIN_FindWndPtr(hWnd);
    WND                   *wndTemp         = WIN_FindWndPtr(hwndActive);
    CBTACTIVATESTRUCT      cbtStruct       = { fMouse , hwndActive };
    FARPROC                enumCallback    = MODULE_GetWndProcEntry16("ActivateAppProc");
    ACTIVATESTRUCT         actStruct;
    WORD                   wIconized=0,wRet= 0;

    /* FIXME: When proper support for cooperative multitasking is in place 
     *        hActiveQ will be global 
     */

    HANDLE                 hActiveQ = 0;   

    /* paranoid checks */
    if( !hWnd || hWnd == GetDesktopWindow() || hWnd == hwndActive )
	return 0;

    if( GetTaskQueue(0) != wndPtr->hmemTaskQ )
	return 0;

    if( wndTemp )
	wIconized = HIWORD(wndTemp->dwStyle & WS_MINIMIZE);
    else
	dprintf_win(stddeb,"WINPOS_ActivateWindow: no current active window.\n");

    /* call CBT hook chain */
    wRet = HOOK_CallHooks(WH_CBT, HCBT_ACTIVATE, (WPARAM)hWnd,
			  (LPARAM)MAKE_SEGPTR(&cbtStruct));

    if( wRet ) return wRet;

    /* set prev active wnd to current active wnd and send notification */
    if( (hwndPrevActive = hwndActive) )
    {
/* FIXME: need a Win32 translation for WINELIB32 */
	if( !SendMessage(hwndPrevActive, WM_NCACTIVATE, 0, MAKELONG(hWnd,wIconized)) )
        {
	    if (GetSysModalWindow() != hWnd) return 0;
	    /* disregard refusal if hWnd is sysmodal */
        }

#ifdef WINELIB32
	SendMessage( hwndActive, WM_ACTIVATE,
		     MAKEWPARAM( WA_INACTIVE, wIconized ),
		     (LPARAM)hWnd );
#else
	SendMessage(hwndPrevActive, WM_ACTIVATE, WA_INACTIVE, 
		    MAKELONG(hWnd,wIconized));
#endif

	/* check if something happened during message processing */
	if( hwndPrevActive != hwndActive ) return 0;
    }

    /* set active wnd */
    hwndActive = hWnd;

    /* send palette messages */
    if( SendMessage( hWnd, WM_QUERYNEWPALETTE, 0, 0L) )
	SendMessage((HWND)-1, WM_PALETTEISCHANGING, (WPARAM)hWnd, 0L );

    /* if prev wnd is minimized redraw icon title 
  if( hwndPrevActive )
    {
        wndTemp = WIN_FindWndPtr( WIN_GetTopParent( hwndPrevActive ) );
        if(wndTemp)
          if(wndTemp->dwStyle & WS_MINIMIZE)
            RedrawIconTitle(hwndPrevActive); 
      } 
  */
    if (!(wndPtr->dwStyle & WS_CHILD))
    {
	/* check Z-order and bring hWnd to the top */
	for (wndTemp = WIN_GetDesktop()->child; wndTemp; wndTemp = wndTemp->next)
	    if (wndTemp->dwStyle & WS_VISIBLE) break;

	if( wndTemp != wndPtr )
	    SetWindowPos(hWnd, HWND_TOP, 0,0,0,0, 
			 SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE );
    }

    if( !IsWindow(hWnd) ) return 0;

    if (hwndPrevActive)
    {
        wndTemp = WIN_FindWndPtr( hwndPrevActive );
        if (wndTemp) hActiveQ = wndTemp->hmemTaskQ;
    }

    /* send WM_ACTIVATEAPP if necessary */
    if (hActiveQ != wndPtr->hmemTaskQ)
    {
	HTASK hT = QUEUE_GetQueueTask( hActiveQ );

	actStruct.wFlag = 0;                  /* deactivate */
	actStruct.hWindowTask = QUEUE_GetQueueTask(wndPtr->hmemTaskQ);
	actStruct.hTaskSendTo = hT;

	/* send WM_ACTIVATEAPP to top-level windows
	 * that belong to the actStruct.hTaskSendTo task
	 */
	EnumWindows( enumCallback , (LPARAM)&actStruct );

	actStruct.wFlag = 1;                  /* activate */
	actStruct.hWindowTask = hT;
	actStruct.hTaskSendTo = QUEUE_GetQueueTask( wndPtr->hmemTaskQ );

	EnumWindows( enumCallback , (LPARAM)&actStruct );

	if( !IsWindow(hWnd) ) return 0;
    }

    /* walk up to the first unowned window */
    wndTemp = wndPtr;
    while (wndTemp->owner) wndTemp = wndTemp->owner;
    /* and set last active owned popup */
    wndTemp->hwndLastActive = hWnd;

    wIconized = HIWORD(wndTemp->dwStyle & WS_MINIMIZE);
/* FIXME: Needs a Win32 translation for WINELIB32 */
    SendMessage( hWnd, WM_NCACTIVATE, 1,
		 MAKELONG(hwndPrevActive,wIconized));
#ifdef WINELIB32
    SendMessage( hWnd, WM_ACTIVATE,
		 MAKEWPARAM( (fMouse)?WA_CLICKACTIVE:WA_ACTIVE, wIconized),
		 (LPARAM)hwndPrevActive );
#else
    SendMessage( hWnd, WM_ACTIVATE, (fMouse)? WA_CLICKACTIVE : WA_ACTIVE,
		 MAKELONG(hwndPrevActive,wIconized));
#endif

    if( !IsWindow(hWnd) ) return 0;

    /* change focus if possible */
    if( fChangeFocus && GetFocus() )
	if( WIN_GetTopParent(GetFocus()) != hwndActive )
	    FOCUS_SwitchFocus( GetFocus(),
			       (wndPtr->dwStyle & WS_MINIMIZE)? 0: hwndActive);

    /* if active wnd is minimized redraw icon title 
  if( hwndActive )
      {
        wndPtr = WIN_FindWndPtr(hwndActive);
        if(wndPtr->dwStyle & WS_MINIMIZE)
           RedrawIconTitle(hwndActive);
    }
  */
    return (hWnd == hwndActive);
}


/*******************************************************************
 *	   WINPOS_ChangeActiveWindow
 *
 */
BOOL WINPOS_ChangeActiveWindow( HWND hWnd, BOOL mouseMsg )
{
    WND *wndPtr = WIN_FindWndPtr(hWnd);

    if( !wndPtr ) return FALSE;

    /* child windows get WM_CHILDACTIVATE message */
    if( (wndPtr->dwStyle & WS_CHILD) && !( wndPtr->dwStyle & WS_POPUP))
	return SendMessage(hWnd, WM_CHILDACTIVATE, 0, 0L);

        /* owned popups imply owner activation */
    if( wndPtr->dwStyle & WS_POPUP && wndPtr->owner )
      {
        wndPtr = wndPtr->owner;
        if( !wndPtr ) return FALSE;
	hWnd = wndPtr->hwndSelf;
      }

    if( hWnd == hwndActive ) return FALSE;

    if( !WINPOS_SetActiveWindow(hWnd ,mouseMsg ,TRUE) )
	return FALSE;

    /* switch desktop queue to current active */
    if( wndPtr->parent == WIN_GetDesktop())
        WIN_GetDesktop()->hmemTaskQ = wndPtr->hmemTaskQ;

    return TRUE;
}


/***********************************************************************
 *           WINPOS_SendNCCalcSize
 *
 * Send a WM_NCCALCSIZE message to a window.
 * All parameters are read-only except newClientRect.
 * oldWindowRect, oldClientRect and winpos must be non-NULL only
 * when calcValidRect is TRUE.
 */
LONG WINPOS_SendNCCalcSize( HWND hwnd, BOOL calcValidRect, RECT *newWindowRect,
			    RECT *oldWindowRect, RECT *oldClientRect,
			    SEGPTR winpos, RECT *newClientRect )
{
    NCCALCSIZE_PARAMS params;
    LONG result;

    params.rgrc[0] = *newWindowRect;
    if (calcValidRect)
    {
	params.rgrc[1] = *oldWindowRect;
	params.rgrc[2] = *oldClientRect;
	params.lppos = winpos;
    }
    result = SendMessage( hwnd, WM_NCCALCSIZE, calcValidRect,
                          (LPARAM)MAKE_SEGPTR( &params ) );
    dprintf_win(stddeb, "WINPOS_SendNCCalcSize: %d %d %d %d\n",
		(int)params.rgrc[0].top,    (int)params.rgrc[0].left,
		(int)params.rgrc[0].bottom, (int)params.rgrc[0].right);
    *newClientRect = params.rgrc[0];
    return result;
}


/***********************************************************************
 *           WINPOS_HandleWindowPosChanging
 *
 * Default handling for a WM_WINDOWPOSCHANGING. Called from DefWindowProc().
 */
LONG WINPOS_HandleWindowPosChanging( WINDOWPOS *winpos )
{
    POINT maxSize;
    WND *wndPtr = WIN_FindWndPtr( winpos->hwnd );
    if (!wndPtr || (winpos->flags & SWP_NOSIZE)) return 0;
    if ((wndPtr->dwStyle & WS_THICKFRAME) ||
	((wndPtr->dwStyle & (WS_POPUP | WS_CHILD)) == 0))
    {
	NC_GetMinMaxInfo( winpos->hwnd, &maxSize, NULL, NULL, NULL );
	winpos->cx = MIN( winpos->cx, maxSize.x );
	winpos->cy = MIN( winpos->cy, maxSize.y );
    }
    return 0;
}


/***********************************************************************
 *           WINPOS_MoveWindowZOrder
 *
 * Move a window in Z order, invalidating everything that needs it.
 * Only necessary for windows without associated X window.
 */
static void WINPOS_MoveWindowZOrder( HWND hwnd, HWND hwndAfter )
{
    BOOL movingUp;
    WND *pWndAfter, *pWndCur, *wndPtr = WIN_FindWndPtr( hwnd );

    /* We have two possible cases:
     * - The window is moving up: we have to invalidate all areas
     *   of the window that were covered by other windows
     * - The window is moving down: we have to invalidate areas
     *   of other windows covered by this one.
     */

    if (hwndAfter == HWND_TOP)
    {
        movingUp = TRUE;
    }
    else if (hwndAfter == HWND_BOTTOM)
    {
        if (!wndPtr->next) return;  /* Already at the bottom */
        movingUp = FALSE;
    }
    else
    {
        if (!(pWndAfter = WIN_FindWndPtr( hwndAfter ))) return;
        if (wndPtr->next == pWndAfter) return;  /* Already placed right */

          /* Determine which window we encounter first in Z-order */
        pWndCur = wndPtr->parent->child;
        while ((pWndCur != wndPtr) && (pWndCur != pWndAfter))
            pWndCur = pWndCur->next;
        movingUp = (pWndCur == pWndAfter);
    }

    if (movingUp)
    {
        WND *pWndPrevAfter = wndPtr->next;
        WIN_UnlinkWindow( hwnd );
        WIN_LinkWindow( hwnd, hwndAfter );
        pWndCur = wndPtr->next;
        while (pWndCur != pWndPrevAfter)
        {
            RECT rect = pWndCur->rectWindow;
            OffsetRect( &rect, -wndPtr->rectClient.left,
                        -wndPtr->rectClient.top );
            RedrawWindow( hwnd, &rect, 0, RDW_INVALIDATE | RDW_ALLCHILDREN |
                          RDW_FRAME | RDW_ERASE );
            pWndCur = pWndCur->next;
        }
    }
    else  /* Moving down */
    {
        pWndCur = wndPtr->next;
        WIN_UnlinkWindow( hwnd );
        WIN_LinkWindow( hwnd, hwndAfter );
        while (pWndCur != wndPtr)
        {
            RECT rect = wndPtr->rectWindow;
            OffsetRect( &rect, -pWndCur->rectClient.left,
                        -pWndCur->rectClient.top );
            RedrawWindow( pWndCur->hwndSelf, &rect, 0, RDW_INVALIDATE |
                          RDW_ALLCHILDREN | RDW_FRAME | RDW_ERASE );
            pWndCur = pWndCur->next;
        }
    }
}

/***********************************************************************
 *           WINPOS_ReorderOwnedPopups
 *
 * fix Z order taking into account owned popups -
 * basically we need to maintain them above owner window
 */
HWND WINPOS_ReorderOwnedPopups(HWND hwndInsertAfter, WND* wndPtr, WORD flags)
{
 WND* 	w = WIN_GetDesktop();

 w = w->child;

 /* if we are dealing with owned popup... 
  */
 if( wndPtr->dwStyle & WS_POPUP && wndPtr->owner && hwndInsertAfter != HWND_TOP )
   {
     BOOL bFound = FALSE;
     HWND hwndLocalPrev = HWND_TOP;
     HWND hwndNewAfter = 0;

     while( w )
       {
         if( !bFound && hwndInsertAfter == hwndLocalPrev )
             hwndInsertAfter = HWND_TOP;

         if( w->dwStyle & WS_POPUP && w->owner == wndPtr->owner )
           {
             bFound = TRUE;

             if( hwndInsertAfter == HWND_TOP )
               {
                 hwndInsertAfter = hwndLocalPrev;
                 break;
               }
             hwndNewAfter = hwndLocalPrev;
           }

         if( w == wndPtr->owner )
           {
             /* basically HWND_BOTTOM */
             hwndInsertAfter = hwndLocalPrev;

             if( bFound )
                 hwndInsertAfter = hwndNewAfter;
             break;
           }

           if( w != wndPtr )
               hwndLocalPrev = w->hwndSelf;

           w = w->next;
        }
   }
 else 
   /* or overlapped top-level window... 
    */
   if( !(wndPtr->dwStyle & WS_CHILD) )
      while( w )
        {
          if( w == wndPtr ) break;

          if( w->dwStyle & WS_POPUP && w->owner == wndPtr )
            {
              SetWindowPos(w->hwndSelf, hwndInsertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                                        SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_DEFERERASE);
              hwndInsertAfter = w->hwndSelf;
            }
          w = w->next;
        }

  return hwndInsertAfter;
}

/***********************************************************************
 *	     WINPOS_SizeMoveClean
 *
 * Make window look nice without excessive repainting
 *
 * the pain:
 *
 * visible regions are in window coordinates
 * update regions are in window client coordinates
 * client and window rectangles are in parent client coordinates
 */
static void WINPOS_SizeMoveClean(WND* Wnd, HRGN oldVisRgn, LPRECT lpOldWndRect, LPRECT lpOldClientRect, BOOL bNoCopy )
{
 HRGN newVisRgn    = DCE_GetVisRgn(Wnd->hwndSelf, DCX_WINDOW | DCX_CLIPSIBLINGS );
 HRGN dirtyRgn     = CreateRectRgn(0,0,0,0);
 int  other, my;

 dprintf_win(stddeb,"cleaning up...new wnd=(%i %i-%i %i) old wnd=(%i %i-%i %i)\n\
\t\tnew client=(%i %i-%i %i) old client=(%i %i-%i %i)\n",
		   Wnd->rectWindow.left, Wnd->rectWindow.top, Wnd->rectWindow.right, Wnd->rectWindow.bottom,
		   lpOldWndRect->left, lpOldWndRect->top, lpOldWndRect->right, lpOldWndRect->bottom,
		   Wnd->rectClient.left,Wnd->rectClient.top,Wnd->rectClient.right,Wnd->rectClient.bottom,
		   lpOldClientRect->left,lpOldClientRect->top,lpOldClientRect->right,lpOldClientRect->bottom);

 CombineRgn( dirtyRgn, newVisRgn, 0, RGN_COPY);

 if( !bNoCopy )
   {
     HRGN hRgn = CreateRectRgn( lpOldClientRect->left - lpOldWndRect->left, lpOldClientRect->top - lpOldWndRect->top,
				lpOldClientRect->right - lpOldWndRect->left, lpOldClientRect->bottom - lpOldWndRect->top);
     CombineRgn( newVisRgn, newVisRgn, oldVisRgn, RGN_AND ); 
     CombineRgn( newVisRgn, newVisRgn, hRgn, RGN_AND );
     DeleteObject(hRgn);
   }

 /* map regions to the parent client area */
 
 OffsetRgn(dirtyRgn, Wnd->rectWindow.left, Wnd->rectWindow.top);
 OffsetRgn(oldVisRgn, lpOldWndRect->left, lpOldWndRect->top);

 /* compute invalidated region outside Wnd - (in client coordinates of the parent window) */

 other = CombineRgn(dirtyRgn, oldVisRgn, dirtyRgn, RGN_DIFF);

 /* map visible region to the Wnd client area */

 OffsetRgn( newVisRgn, Wnd->rectWindow.left - Wnd->rectClient.left,
                       Wnd->rectWindow.top - Wnd->rectClient.top );

 /* substract previously invalidated region from the Wnd visible region */

 my =  (Wnd->hrgnUpdate > 1)? CombineRgn( newVisRgn, newVisRgn, Wnd->hrgnUpdate, RGN_DIFF)
                            : COMPLEXREGION;

 if( bNoCopy )		/* invalidate Wnd visible region */
   {
     if (my != NULLREGION)  RedrawWindow( Wnd->hwndSelf, NULL, newVisRgn, RDW_INVALIDATE |
		            RDW_FRAME | RDW_ALLCHILDREN | RDW_ERASE );
   } 
 else			/* bitblt old client area */
   { 
     HDC   hDC;
     int   update;
     HRGN  updateRgn;

     /* client rect */

     updateRgn = CreateRectRgn( 0,0, Wnd->rectClient.right - Wnd->rectClient.left,
				Wnd->rectClient.bottom - Wnd->rectClient.top );

     /* clip visible region with client rect */

     my = CombineRgn( newVisRgn, newVisRgn, updateRgn, RGN_AND );

     /* substract result from client rect to get region that won't be copied */

     update = CombineRgn( updateRgn, updateRgn, newVisRgn, RGN_DIFF );

     /* Blt valid bits using parent window DC */

     if( my != NULLREGION )
       {
	 int xfrom = lpOldClientRect->left;
	 int yfrom = lpOldClientRect->top;
	 int xto = Wnd->rectClient.left;
	 int yto = Wnd->rectClient.top;

	 /* check if we can skip copying */

	 if( xfrom != xto || yfrom != yto )
	   {
	     /* compute clipping region in parent client coordinates */

	     OffsetRgn( newVisRgn, Wnd->rectClient.left, Wnd->rectClient.top);
	     CombineRgn( oldVisRgn, oldVisRgn, newVisRgn, RGN_OR );

             hDC = GetDCEx( Wnd->parent->hwndSelf, oldVisRgn, DCX_INTERSECTRGN | DCX_CACHE | DCX_CLIPSIBLINGS);

             BitBlt(hDC, xto, yto, lpOldClientRect->right - lpOldClientRect->left + 1, 
				   lpOldClientRect->bottom - lpOldClientRect->top + 1,
				   hDC, xfrom, yfrom, SRCCOPY );
    
             ReleaseDC( Wnd->parent->hwndSelf, hDC); 
	  }
       }

     if( update != NULLREGION )
         RedrawWindow( Wnd->hwndSelf, NULL, updateRgn, RDW_INVALIDATE |
 				      RDW_FRAME | RDW_ALLCHILDREN | RDW_ERASE );
     DeleteObject( updateRgn );
   }

 /* erase uncovered areas */

 if( other != NULLREGION )
     RedrawWindow( Wnd->parent->hwndSelf, NULL, dirtyRgn,
		   RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE );

 DeleteObject(dirtyRgn);
 DeleteObject(newVisRgn);
}

/***********************************************************************
 *           WINPOS_SetXWindowPos
 *
 * SetWindowPos() for an X window. Used by the real SetWindowPos().
 */
static void WINPOS_SetXWindowPos( WINDOWPOS *winpos )
{
    XWindowChanges winChanges;
    int changeMask = 0;
    WND *wndPtr = WIN_FindWndPtr( winpos->hwnd );

    if (!(winpos->flags & SWP_NOSIZE))
    {
        winChanges.width     = winpos->cx;
        winChanges.height    = winpos->cy;
        changeMask |= CWWidth | CWHeight;
    }
    if (!(winpos->flags & SWP_NOMOVE))
    {
        winChanges.x = winpos->x;
        winChanges.y = winpos->y;
        changeMask |= CWX | CWY;
    }
    if (!(winpos->flags & SWP_NOZORDER))
    {
        if (winpos->hwndInsertAfter == HWND_TOP) winChanges.stack_mode = Above;
        else winChanges.stack_mode = Below;
        if ((winpos->hwndInsertAfter != HWND_TOP) &&
            (winpos->hwndInsertAfter != HWND_BOTTOM))
        {
            WND * insertPtr = WIN_FindWndPtr( winpos->hwndInsertAfter );
            winChanges.sibling = insertPtr->window;
            changeMask |= CWSibling;
        }
        changeMask |= CWStackMode;
    }
    if (changeMask)
        XConfigureWindow( display, wndPtr->window, changeMask, &winChanges );
}


/***********************************************************************
 *           SetWindowPos   (USER.232)
 */
BOOL SetWindowPos( HWND hwnd, HWND hwndInsertAfter, INT x, INT y,
		   INT cx, INT cy, WORD flags )
{
    WINDOWPOS 	winpos;
    WND *	wndPtr;
    RECT 	newWindowRect, newClientRect;
    HRGN	visRgn = 0;
    int 	result = 0;

    dprintf_win(stddeb,"SetWindowPos: hwnd %04x, flags %08x\n", hwnd, flags);  

      /* Check window handle */

    if (hwnd == GetDesktopWindow()) return FALSE;
    if (!(wndPtr = WIN_FindWndPtr( hwnd ))) return FALSE;

    /* Check for windows that may not be resized 
       FIXME: this should be done only for Windows 3.0 programs */
    if (flags ==(SWP_SHOWWINDOW) || flags ==(SWP_HIDEWINDOW ) )
       flags |= SWP_NOSIZE | SWP_NOMOVE;

      /* Check dimensions */

    if (cx <= 0) cx = 1;
    if (cy <= 0) cy = 1;

      /* Check flags */

    if (hwnd == hwndActive) flags |= SWP_NOACTIVATE;   /* Already active */
    if ((wndPtr->rectWindow.right - wndPtr->rectWindow.left == cx) &&
        (wndPtr->rectWindow.bottom - wndPtr->rectWindow.top == cy))
        flags |= SWP_NOSIZE;    /* Already the right size */
    if ((wndPtr->rectWindow.left == x) && (wndPtr->rectWindow.top == y))
        flags |= SWP_NOMOVE;    /* Already the right position */

      /* Check hwndInsertAfter */

    if (!(flags & (SWP_NOZORDER | SWP_NOACTIVATE)))
    {
	  /* Ignore TOPMOST flags when activating a window */
          /* _and_ moving it in Z order. */
	if ((hwndInsertAfter == HWND_TOPMOST) ||
            (hwndInsertAfter == HWND_NOTOPMOST))
	    hwndInsertAfter = HWND_TOP;	
    }
      /* TOPMOST not supported yet */
    if ((hwndInsertAfter == HWND_TOPMOST) ||
        (hwndInsertAfter == HWND_NOTOPMOST)) hwndInsertAfter = HWND_TOP;

      /* hwndInsertAfter must be a sibling of the window */
    if ((hwndInsertAfter != HWND_TOP) && (hwndInsertAfter != HWND_BOTTOM))
       {
	 WND* wnd = WIN_FindWndPtr(hwndInsertAfter);
	 if( wnd->parent != wndPtr->parent ) return FALSE;
	 if( wnd->next == wndPtr ) flags |= SWP_NOZORDER;
       }
    else
       if (hwndInsertAfter == HWND_TOP)
	   flags |= ( wndPtr->parent->child == wndPtr)? SWP_NOZORDER: 0;
       else /* HWND_BOTTOM */
	   flags |= ( wndPtr->next )? 0: SWP_NOZORDER;

      /* Fill the WINDOWPOS structure */

    winpos.hwnd = hwnd;
    winpos.hwndInsertAfter = hwndInsertAfter;
    winpos.x = x;
    winpos.y = y;
    winpos.cx = cx;
    winpos.cy = cy;
    winpos.flags = flags;
    
      /* Send WM_WINDOWPOSCHANGING message */

    if (!(flags & SWP_NOSENDCHANGING))
	SendMessage( hwnd, WM_WINDOWPOSCHANGING, 0, (LPARAM)MAKE_SEGPTR(&winpos) );

      /* Calculate new position and size */

    newWindowRect = wndPtr->rectWindow;
    newClientRect = wndPtr->rectClient;

    if (!(winpos.flags & SWP_NOSIZE))
    {
        newWindowRect.right  = newWindowRect.left + winpos.cx;
        newWindowRect.bottom = newWindowRect.top + winpos.cy;
    }
    if (!(winpos.flags & SWP_NOMOVE))
    {
        newWindowRect.left    = winpos.x;
        newWindowRect.top     = winpos.y;
        newWindowRect.right  += winpos.x - wndPtr->rectWindow.left;
        newWindowRect.bottom += winpos.y - wndPtr->rectWindow.top;

	OffsetRect(&newClientRect, winpos.x - wndPtr->rectWindow.left, 
				   winpos.y - wndPtr->rectWindow.top );
    }

    winpos.flags |= SWP_NOCLIENTMOVE | SWP_NOCLIENTSIZE;

      /* Reposition window in Z order */

    if (!(winpos.flags & SWP_NOZORDER))
    {
	/* reorder owned popups if hwnd is top-level window 
         */
	if( wndPtr->parent == WIN_GetDesktop() )
	    hwndInsertAfter = WINPOS_ReorderOwnedPopups( hwndInsertAfter,
							 wndPtr, flags );

        if (wndPtr->window)
        {
            WIN_UnlinkWindow( winpos.hwnd );
            WIN_LinkWindow( winpos.hwnd, hwndInsertAfter );
        }
        else WINPOS_MoveWindowZOrder( winpos.hwnd, hwndInsertAfter );
    }

    if ( !wndPtr->window && !(flags & SWP_NOREDRAW) && 
        (!(flags & SWP_NOMOVE) || !(flags & SWP_NOSIZE) || (flags & SWP_FRAMECHANGED)) )
          visRgn = DCE_GetVisRgn(hwnd, DCX_WINDOW | DCX_CLIPSIBLINGS);


      /* Send WM_NCCALCSIZE message to get new client area */
    if( (flags & (SWP_FRAMECHANGED | SWP_NOSIZE)) != SWP_NOSIZE )
      {
         result = WINPOS_SendNCCalcSize( winpos.hwnd, TRUE, &newWindowRect,
				    &wndPtr->rectWindow, &wndPtr->rectClient,
				    MAKE_SEGPTR(&winpos), &newClientRect );

         /* FIXME: WVR_ALIGNxxx */

         if( newClientRect.left != wndPtr->rectClient.left ||
             newClientRect.top != wndPtr->rectClient.top )
             winpos.flags &= ~SWP_NOCLIENTMOVE;

         if( (newClientRect.right - newClientRect.left !=
             wndPtr->rectClient.right - wndPtr->rectClient.left) ||
  	    (newClientRect.bottom - newClientRect.top !=
	     wndPtr->rectClient.bottom - wndPtr->rectClient.top) )
	     winpos.flags &= ~SWP_NOCLIENTSIZE;
      }
    else
      if( !(flags & SWP_NOMOVE) && (newClientRect.left != wndPtr->rectClient.left ||
				    newClientRect.top != wndPtr->rectClient.top) )
	    winpos.flags &= ~SWP_NOCLIENTMOVE;

    /* Perform the moving and resizing */

    if (wndPtr->window)
    {
        RECT oldWindowRect = wndPtr->rectWindow;
        RECT oldClientRect = wndPtr->rectClient;

        HWND bogusInsertAfter = winpos.hwndInsertAfter;

        winpos.hwndInsertAfter = hwndInsertAfter;
        WINPOS_SetXWindowPos( &winpos );

        wndPtr->rectWindow = newWindowRect;
        wndPtr->rectClient = newClientRect;
        winpos.hwndInsertAfter = bogusInsertAfter;

	/*  FIXME: should do something like WINPOS_SizeMoveClean */

	if( (oldClientRect.left - oldWindowRect.left !=
	     newClientRect.left - newWindowRect.left) ||
	    (oldClientRect.top - oldWindowRect.top !=
	     newClientRect.top - newWindowRect.top) )

	    RedrawWindow(wndPtr->hwndSelf, NULL, 0, RDW_ALLCHILDREN | RDW_FRAME | RDW_ERASE);
	else
	    if( winpos.flags & SWP_FRAMECHANGED )
	      {
		WORD wErase = 0;
		RECT rect;

	        if( oldClientRect.right > newClientRect.right ) 
		  {
		    rect.left = newClientRect.right; rect.top = newClientRect.top;
		    rect.right = oldClientRect.right; rect.bottom = newClientRect.bottom;
		    wErase = 1;
		    RedrawWindow(wndPtr->hwndSelf, &rect, 0, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
		  }
		if( oldClientRect.bottom > newClientRect.bottom )
		  {
		    rect.left = newClientRect.left; rect.top = newClientRect.bottom;
		    rect.right = (wErase)?oldClientRect.right:newClientRect.right;
		    rect.bottom = oldClientRect.bottom;
		    wErase = 1;
		    RedrawWindow(wndPtr->hwndSelf, &rect, 0, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
		  }

		if( !wErase ) wndPtr->flags |= WIN_NEEDS_NCPAINT;
	      }
    }
    else
    {
        RECT oldWindowRect = wndPtr->rectWindow;
	RECT oldClientRect = wndPtr->rectClient;

        wndPtr->rectWindow = newWindowRect;
        wndPtr->rectClient = newClientRect;

        if( !(flags & SWP_NOREDRAW) )
	  {
	    BOOL bNoCopy = (flags & SWP_NOCOPYBITS) || 
			   (result >= WVR_HREDRAW && result < WVR_VALIDRECTS);

	    if( (winpos.flags & SWP_NOPOSCHANGE) != SWP_NOPOSCHANGE )
	      {
	        /* optimize cleanup by BitBlt'ing where possible */

	        WINPOS_SizeMoveClean(wndPtr, visRgn, &oldWindowRect, &oldClientRect, bNoCopy);
	        DeleteObject(visRgn);
	      }
	    else
	       if( winpos.flags & SWP_FRAMECHANGED )
        	  RedrawWindow( winpos.hwnd, NULL, 0, RDW_NOCHILDREN | RDW_FRAME ); 

	  }
        DeleteObject(visRgn);
    }

    if (flags & SWP_SHOWWINDOW)
    {
	wndPtr->dwStyle |= WS_VISIBLE;
        if (wndPtr->window)
        {
            XMapWindow( display, wndPtr->window );
        }
        else
        {
            if (!(flags & SWP_NOREDRAW))
                RedrawWindow( winpos.hwnd, NULL, 0,
                              RDW_INVALIDATE | RDW_ALLCHILDREN |
			      RDW_FRAME | RDW_ERASE );
        }
    }
    else if (flags & SWP_HIDEWINDOW)
    {
	wndPtr->dwStyle &= ~WS_VISIBLE;
        if (wndPtr->window)
        {
            XUnmapWindow( display, wndPtr->window );
        }
        else
        {
            if (!(flags & SWP_NOREDRAW))
                RedrawWindow( wndPtr->parent->hwndSelf, &wndPtr->rectWindow, 0,
                              RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE );
        }

        if ((winpos.hwnd == GetFocus()) || IsChild(winpos.hwnd, GetFocus()))
            SetFocus( GetParent(winpos.hwnd) );  /* Revert focus to parent */

	if (winpos.hwnd == hwndActive)
	{
	      /* Activate previously active window if possible */
	    HWND newActive = hwndPrevActive;
	    if (!IsWindow(newActive) || (newActive == winpos.hwnd))
	    {
		newActive = GetTopWindow( GetDesktopWindow() );
		if (newActive == winpos.hwnd)
                    newActive = wndPtr->next ? wndPtr->next->hwndSelf : 0;
	    }	    
	    WINPOS_ChangeActiveWindow( newActive, FALSE );
	}
    }

      /* Activate the window */

    if (!(flags & SWP_NOACTIVATE))
	    WINPOS_ChangeActiveWindow( winpos.hwnd, FALSE );
    
      /* Repaint the window */

    if (wndPtr->window) MSG_Synchronize();  /* Wait for all expose events */

    EVENT_DummyMotionNotify(); /* Simulate a mouse event to set the cursor */

    if (!(flags & SWP_DEFERERASE))
        RedrawWindow( wndPtr->parent->hwndSelf, NULL, 0,
                      RDW_ALLCHILDREN | RDW_ERASENOW );

      /* And last, send the WM_WINDOWPOSCHANGED message */

    if (!(winpos.flags & SWP_NOSENDCHANGING))
        SendMessage( winpos.hwnd, WM_WINDOWPOSCHANGED,
                     0, (LPARAM)MAKE_SEGPTR(&winpos) );

    return TRUE;
}

					
/***********************************************************************
 *           BeginDeferWindowPos   (USER.259)
 */
HDWP BeginDeferWindowPos( INT count )
{
    HDWP handle;
    DWP *pDWP;

    if (count <= 0) return 0;
    handle = USER_HEAP_ALLOC( sizeof(DWP) + (count-1)*sizeof(WINDOWPOS) );
    if (!handle) return 0;
    pDWP = (DWP *) USER_HEAP_LIN_ADDR( handle );
    pDWP->actualCount    = 0;
    pDWP->suggestedCount = count;
    pDWP->valid          = TRUE;
    pDWP->wMagic         = DWP_MAGIC;
    pDWP->hwndParent     = 0;
    return handle;
}


/***********************************************************************
 *           DeferWindowPos   (USER.260)
 */
HDWP DeferWindowPos( HDWP hdwp, HWND hwnd, HWND hwndAfter, INT x, INT y,
                     INT cx, INT cy, UINT flags )
{
    DWP *pDWP;
    int i;
    HDWP newhdwp = hdwp;
    HWND parent;

    pDWP = (DWP *) USER_HEAP_LIN_ADDR( hdwp );
    if (!pDWP) return 0;
    if (hwnd == GetDesktopWindow()) return 0;

      /* All the windows of a DeferWindowPos() must have the same parent */

    parent = WIN_FindWndPtr( hwnd )->parent->hwndSelf;
    if (pDWP->actualCount == 0) pDWP->hwndParent = parent;
    else if (parent != pDWP->hwndParent)
    {
        USER_HEAP_FREE( hdwp );
        return 0;
    }

    for (i = 0; i < pDWP->actualCount; i++)
    {
        if (pDWP->winPos[i].hwnd == hwnd)
        {
              /* Merge with the other changes */
            if (!(flags & SWP_NOZORDER))
            {
                pDWP->winPos[i].hwndInsertAfter = hwndAfter;
            }
            if (!(flags & SWP_NOMOVE))
            {
                pDWP->winPos[i].x = x;
                pDWP->winPos[i].y = y;
            }                
            if (!(flags & SWP_NOSIZE))
            {
                pDWP->winPos[i].cx = cx;
                pDWP->winPos[i].cy = cy;
            }
            pDWP->winPos[i].flags &= flags & (SWP_NOSIZE | SWP_NOMOVE |
                                              SWP_NOZORDER | SWP_NOREDRAW |
                                              SWP_NOACTIVATE | SWP_NOCOPYBITS |
                                              SWP_NOOWNERZORDER);
            pDWP->winPos[i].flags |= flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW |
                                              SWP_FRAMECHANGED);
            return hdwp;
        }
    }
    if (pDWP->actualCount >= pDWP->suggestedCount)
    {
        newhdwp = USER_HEAP_REALLOC( hdwp,
                      sizeof(DWP) + pDWP->suggestedCount*sizeof(WINDOWPOS) );
        if (!newhdwp) return 0;
        pDWP = (DWP *) USER_HEAP_LIN_ADDR( newhdwp );
        pDWP->suggestedCount++;
    }
    pDWP->winPos[pDWP->actualCount].hwnd = hwnd;
    pDWP->winPos[pDWP->actualCount].hwndInsertAfter = hwndAfter;
    pDWP->winPos[pDWP->actualCount].x = x;
    pDWP->winPos[pDWP->actualCount].y = y;
    pDWP->winPos[pDWP->actualCount].cx = cx;
    pDWP->winPos[pDWP->actualCount].cy = cy;
    pDWP->winPos[pDWP->actualCount].flags = flags;
    pDWP->actualCount++;
    return newhdwp;
}


/***********************************************************************
 *           EndDeferWindowPos   (USER.261)
 */
BOOL EndDeferWindowPos( HDWP hdwp )
{
    DWP *pDWP;
    WINDOWPOS *winpos;
    BOOL res = TRUE;
    int i;

    pDWP = (DWP *) USER_HEAP_LIN_ADDR( hdwp );
    if (!pDWP) return FALSE;
    for (i = 0, winpos = pDWP->winPos; i < pDWP->actualCount; i++, winpos++)
    {
        if (!(res = SetWindowPos( winpos->hwnd, winpos->hwndInsertAfter,
                                  winpos->x, winpos->y, winpos->cx, winpos->cy,
                                  winpos->flags ))) break;
    }
    USER_HEAP_FREE( hdwp );
    return res;
}


/***********************************************************************
 *           TileChildWindows   (USER.199)
 */
void TileChildWindows( HWND parent, WORD action )
{
    printf("STUB TileChildWindows(%04x, %d)\n", parent, action);
}

/***********************************************************************
 *           CascageChildWindows   (USER.198)
 */
void CascadeChildWindows( HWND parent, WORD action )
{
    printf("STUB CascadeChildWindows(%04x, %d)\n", parent, action);
}
