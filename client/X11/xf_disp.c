/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * X11 Display Control channel
 *
 * Copyright 2017 David Fort <contact@hardening-consulting.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winpr/sysinfo.h>
#include <X11/Xutil.h>

#ifdef WITH_XRANDR
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/randr.h>

#if (RANDR_MAJOR * 100 + RANDR_MINOR) >= 105
#	define USABLE_XRANDR
#endif

#endif

#include "xf_disp.h"
#include "xf_monitor.h"


#define TAG CLIENT_TAG("x11disp")
#define RESIZE_MIN_DELAY 200 /* minimum delay in ms between two resizes */

struct _xfDispContext
{
	xfContext* xfc;
	BOOL haveXRandr;
	int eventBase, errorBase;
	int lastSentWidth, lastSentHeight;
	UINT64 lastSentDate;
	int targetWidth, targetHeight;
	BOOL activated;
	BOOL waitingResize;
	BOOL fullscreen;
	UINT16 lastSentDesktopOrientation;
	UINT32 lastSentDesktopScaleFactor;
	UINT32 lastSentDeviceScaleFactor;
};

static UINT xf_disp_sendLayout(DispClientContext* disp, rdpMonitor* monitors, int nmonitors);

static BOOL xf_disp_settings_changed(xfDispContext* xfDisp)
{
	rdpSettings* settings;
	settings = xfDisp->xfc->context.settings;

	if (xfDisp->lastSentWidth != xfDisp->targetWidth)
		return TRUE;

	if (xfDisp->lastSentHeight != xfDisp->targetHeight)
		return TRUE;

	if (xfDisp->lastSentDesktopOrientation != settings->DesktopOrientation)
		return TRUE;

	if (xfDisp->lastSentDesktopScaleFactor != settings->DesktopScaleFactor)
		return TRUE;

	if (xfDisp->lastSentDeviceScaleFactor != settings->DeviceScaleFactor)
		return TRUE;

	if (xfDisp->fullscreen != xfDisp->xfc->fullscreen)
		return TRUE;

	return FALSE;
}

static BOOL xf_update_last_sent(xfDispContext* xfDisp)
{
	rdpSettings* settings = xfDisp->xfc->context.settings;
	xfDisp->lastSentWidth = xfDisp->targetWidth;
	xfDisp->lastSentHeight = xfDisp->targetHeight;
	xfDisp->lastSentDesktopOrientation = settings->DesktopOrientation;
	xfDisp->lastSentDesktopScaleFactor = settings->DesktopScaleFactor;
	xfDisp->lastSentDeviceScaleFactor = settings->DeviceScaleFactor;
	xfDisp->fullscreen = xfDisp->xfc->fullscreen;
	return TRUE;
}

static BOOL xf_disp_sendResize(xfDispContext* xfDisp)
{
	DISPLAY_CONTROL_MONITOR_LAYOUT layout;
	xfContext* xfc = xfDisp->xfc;
	rdpSettings* settings = xfc->context.settings;

	if (!xfDisp->activated)
		return TRUE;

	if (GetTickCount64() - xfDisp->lastSentDate < RESIZE_MIN_DELAY)
		return TRUE;

	xfDisp->lastSentDate = GetTickCount64();

	if (!xf_disp_settings_changed(xfDisp))
		return TRUE;

	if (xfc->fullscreen && (settings->MonitorCount > 0))
	{
		if (xf_disp_sendLayout(xfc->disp, settings->MonitorDefArray,
		                       settings->MonitorCount) != CHANNEL_RC_OK)
			return FALSE;
	}
	else
	{
		xfDisp->waitingResize = TRUE;
		layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
		layout.Top = layout.Left = 0;
		layout.Width = xfDisp->targetWidth;
		layout.Height = xfDisp->targetHeight;
		layout.Orientation = settings->DesktopOrientation;
		layout.DesktopScaleFactor = settings->DesktopScaleFactor;
		layout.DeviceScaleFactor = settings->DeviceScaleFactor;
		layout.PhysicalWidth = xfDisp->targetWidth;
		layout.PhysicalHeight = xfDisp->targetHeight;

		if (xfc->disp->SendMonitorLayout(xfc->disp, 1, &layout) != CHANNEL_RC_OK)
			return FALSE;
	}

	return xf_update_last_sent(xfDisp);
}


static BOOL xf_disp_set_window_resizable(xfDispContext* xfDisp)
{
	XSizeHints* size_hints;

	if (!(size_hints = XAllocSizeHints()))
		return FALSE;

	size_hints->flags = PMinSize | PMaxSize | PWinGravity;
	size_hints->win_gravity = NorthWestGravity;
	size_hints->min_width = size_hints->min_height = 320;
	size_hints->max_width = size_hints->max_height = 8192;

	if (xfDisp->xfc->window)
		XSetWMNormalHints(xfDisp->xfc->display, xfDisp->xfc->window->handle, size_hints);

	XFree(size_hints);
	return TRUE;
}


static void xf_disp_OnActivated(void* context, ActivatedEventArgs* e)
{
	xfContext* xfc = (xfContext*)context;
	xfDispContext* xfDisp = xfc->xfDisp;
	rdpSettings* settings = xfc->context.settings;
	xfDisp->waitingResize = FALSE;

	if (xfDisp->activated && !settings->Fullscreen)
	{
		xf_disp_set_window_resizable(xfDisp);

		if (e->firstActivation)
			return;

		xf_disp_sendResize(xfDisp);
	}
}


static void xf_disp_OnGraphicsReset(void* context, GraphicsResetEventArgs* e)
{
	xfContext* xfc = (xfContext*)context;
	xfDispContext* xfDisp = xfc->xfDisp;
	rdpSettings* settings = xfc->context.settings;
	xfDisp->waitingResize = FALSE;

	if (xfDisp->activated && !settings->Fullscreen)
	{
		xf_disp_set_window_resizable(xfDisp);
		xf_disp_sendResize(xfDisp);
	}
}

static void xf_disp_OnTimer(void* context, TimerEventArgs* e)
{
	xfContext* xfc = (xfContext*)context;
	xfDispContext* xfDisp = xfc->xfDisp;
	rdpSettings* settings = xfc->context.settings;

	if (!xfDisp->activated || settings->Fullscreen)
		return;

	xf_disp_sendResize(xfDisp);
}

xfDispContext* xf_disp_new(xfContext* xfc)
{
	xfDispContext* ret = calloc(1, sizeof(xfDispContext));

	if (!ret)
		return NULL;

	ret->xfc = xfc;
#ifdef USABLE_XRANDR

	if (XRRQueryExtension(xfc->display, &ret->eventBase, &ret->errorBase))
	{
		ret->haveXRandr = TRUE;
	}

#endif
	ret->lastSentWidth = ret->targetWidth = xfc->context.settings->DesktopWidth;
	ret->lastSentHeight = ret->targetHeight = xfc->context.settings->DesktopHeight;
	PubSub_SubscribeActivated(xfc->context.pubSub, xf_disp_OnActivated);
	PubSub_SubscribeGraphicsReset(xfc->context.pubSub, xf_disp_OnGraphicsReset);
	PubSub_SubscribeTimer(xfc->context.pubSub, xf_disp_OnTimer);
	return ret;
}

void xf_disp_free(xfDispContext* disp)
{
	PubSub_UnsubscribeActivated(disp->xfc->context.pubSub, xf_disp_OnActivated);
	PubSub_UnsubscribeGraphicsReset(disp->xfc->context.pubSub, xf_disp_OnGraphicsReset);
	PubSub_UnsubscribeTimer(disp->xfc->context.pubSub, xf_disp_OnTimer);
	free(disp);
}

static UINT xf_disp_sendLayout(DispClientContext* disp, rdpMonitor* monitors, int nmonitors)
{
	UINT ret = CHANNEL_RC_OK;
	DISPLAY_CONTROL_MONITOR_LAYOUT* layouts;
	int i;
	xfDispContext* xfDisp = (xfDispContext*)disp->custom;
	rdpSettings* settings = xfDisp->xfc->context.settings;
	layouts = calloc(nmonitors, sizeof(DISPLAY_CONTROL_MONITOR_LAYOUT));

	if (!layouts)
		return CHANNEL_RC_NO_MEMORY;

	for (i = 0; i < nmonitors; i++)
	{
		layouts[i].Flags = (monitors[i].is_primary ? DISPLAY_CONTROL_MONITOR_PRIMARY : 0);
		layouts[i].Left = monitors[i].x;
		layouts[i].Top = monitors[i].y;
		layouts[i].Width = monitors[i].width;
		layouts[i].Height = monitors[i].height;
		layouts[i].Orientation = ORIENTATION_LANDSCAPE;
		layouts[i].PhysicalWidth = monitors[i].attributes.physicalWidth;
		layouts[i].PhysicalHeight = monitors[i].attributes.physicalHeight;

		switch (monitors[i].attributes.orientation)
		{
			case 90:
				layouts[i].Orientation = ORIENTATION_PORTRAIT;
				break;

			case 180:
				layouts[i].Orientation = ORIENTATION_LANDSCAPE_FLIPPED;
				break;

			case 270:
				layouts[i].Orientation = ORIENTATION_PORTRAIT_FLIPPED;
				break;

			case 0:
			default:
				/* MS-RDPEDISP - 2.2.2.2.1:
				 * Orientation (4 bytes): A 32-bit unsigned integer that specifies the
				 * orientation of the monitor in degrees. Valid values are 0, 90, 180
				 * or 270
				 *
				 * So we default to ORIENTATION_LANDSCAPE
				 */
				layouts[i].Orientation = ORIENTATION_LANDSCAPE;
				break;
		}

		layouts[i].DesktopScaleFactor = settings->DesktopScaleFactor;
		layouts[i].DeviceScaleFactor = settings->DeviceScaleFactor;
	}

	ret = disp->SendMonitorLayout(disp, nmonitors, layouts);
	free(layouts);
	return ret;
}

BOOL xf_disp_handle_xevent(xfContext* xfc, XEvent* event)
{
	xfDispContext* xfDisp = xfc->xfDisp;
	rdpSettings* settings = xfc->context.settings;
	UINT32 maxWidth, maxHeight;

	if (!xfDisp->haveXRandr)
		return TRUE;

#ifdef USABLE_XRANDR

	if (event->type != xfDisp->eventBase + RRScreenChangeNotify)
		return TRUE;

#endif
	xf_detect_monitors(xfc, &maxWidth, &maxHeight);
	return xf_disp_sendLayout(xfc->disp, settings->MonitorDefArray,
	                          settings->MonitorCount) == CHANNEL_RC_OK;
}


BOOL xf_disp_handle_configureNotify(xfContext* xfc, int width, int height)
{
	xfDispContext* xfDisp = xfc->xfDisp;
	xfDisp->targetWidth = width;
	xfDisp->targetHeight = height;
	return xf_disp_sendResize(xfDisp);
}


UINT xf_DisplayControlCaps(DispClientContext* disp, UINT32 maxNumMonitors,
                           UINT32 maxMonitorAreaFactorA, UINT32 maxMonitorAreaFactorB)
{
	/* we're called only if dynamic resolution update is activated */
	xfDispContext* xfDisp = (xfDispContext*)disp->custom;
	rdpSettings* settings = xfDisp->xfc->context.settings;
	WLog_DBG(TAG,
	         "DisplayControlCapsPdu: MaxNumMonitors: %"PRIu32" MaxMonitorAreaFactorA: %"PRIu32" MaxMonitorAreaFactorB: %"PRIu32"",
	         maxNumMonitors, maxMonitorAreaFactorA, maxMonitorAreaFactorB);
	xfDisp->activated = TRUE;

	if (settings->Fullscreen)
		return CHANNEL_RC_OK;

	WLog_DBG(TAG, "DisplayControlCapsPdu: setting the window as resizeable");
	return xf_disp_set_window_resizable(xfDisp) ? CHANNEL_RC_OK : CHANNEL_RC_NO_MEMORY;
}

BOOL xf_disp_init(xfContext* xfc, DispClientContext* disp)
{
	rdpSettings* settings = xfc->context.settings;
	xfc->disp = disp;
	disp->custom = (void*) xfc->xfDisp;

	if (settings->DynamicResolutionUpdate)
	{
		disp->DisplayControlCaps = xf_DisplayControlCaps;
#ifdef USABLE_XRANDR

		if (settings->Fullscreen)
		{
			/* ask X11 to notify us of screen changes */
			XRRSelectInput(xfc->display, DefaultRootWindow(xfc->display), RRScreenChangeNotifyMask);
		}

#endif
	}

	return TRUE;
}

