/*
gc_cam_cmd.h - `gc_cam <file> <clip>`: GxBake .gcam scripted cameras (gc_cam.c)
*/
#ifndef GC_CAM_CMD_H
#define GC_CAM_CMD_H

void GC_CamInit( void );
/* While a clip runs, overwrite the view with it and return true. Call after the
 * view is set up for the frame (V_CalcRefdef / the New Game camera). */
qboolean GC_CamApplyView( float *vieworg, float *viewangles, float *fov_x );

#endif
