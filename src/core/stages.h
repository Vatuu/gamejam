#ifndef _CC_STAGE_
#define _CC_STAGE_

#include "actor.h"
#include "gfx_management.h"

struct stage {
    const char* stage_id;
    camera* camera;
    void (*stage_init)(void);
    void (*stage_update)(void);
    void (*stage_frame)(void);
};

extern actor_data ACTORS[MAX_ACTORS];

extern void change_stage(struct stage* newStage);

extern void stage_update();
extern void stage_render();

extern struct stage stage00_n64;

#endif