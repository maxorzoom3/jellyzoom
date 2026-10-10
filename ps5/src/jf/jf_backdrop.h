#pragma once
/* The big picture behind a title: the server's "backdrop" image of the film whose page is open.
 * A worker thread fetches and decodes it; the program takes it on the render thread. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int gate_index; /* the title it belongs to */
	int width;
	int height;
	unsigned char *rgba; /* width*height*4 bytes from malloc; whoever takes it frees it with free() */
} JfBackdrop;

/* The title whose backdrop is wanted (a gate index), or -1 for none. Cheap: call it every frame. */
void jf_backdrop_want(int gate_index);
/* 1, and *out filled, when a decoded backdrop is waiting. A title without one is simply never delivered. */
int jf_backdrop_take(JfBackdrop *out);
/* Stops the worker. */
void jf_backdrop_stop(void);

/* Stage 35: the title's logo (Jellyfin's "Logo" image, a transparent picture of the title in its own lettering) for the
 * player. A series' episodes show the series' logo. */
/* The title whose logo is wanted (a gate index), or -1 for none. Cheap: call it every frame. */
void jf_logo_want(int gate_index);
/* 1, and *out filled, when a decoded logo is waiting (free rgba with free()). */
int jf_logo_take(JfBackdrop *out);
/* 0 nothing asked, 1 loading, 2 delivered, -1 the title has no logo */
int jf_logo_state(void);

#ifdef __cplusplus
}
#endif
