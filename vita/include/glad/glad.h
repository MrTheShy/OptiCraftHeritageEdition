#ifndef OPTICRAFT_VITA_GLAD_H
#define OPTICRAFT_VITA_GLAD_H

#include <vitaGL.h>

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB GL_TEXTURE0
#endif
#ifndef GL_TEXTURE1_ARB
#define GL_TEXTURE1_ARB GL_TEXTURE1
#endif

#ifndef GL_S
#define GL_S 0x2000
#endif
#ifndef GL_T
#define GL_T 0x2001
#endif
#ifndef GL_R
#define GL_R 0x2002
#endif
#ifndef GL_Q
#define GL_Q 0x2003
#endif
#ifndef GL_EYE_LINEAR
#define GL_EYE_LINEAR 0x2400
#endif
#ifndef GL_OBJECT_LINEAR
#define GL_OBJECT_LINEAR 0x2401
#endif
#ifndef GL_TEXTURE_GEN_MODE
#define GL_TEXTURE_GEN_MODE 0x2500
#endif
#ifndef GL_OBJECT_PLANE
#define GL_OBJECT_PLANE 0x2501
#endif
#ifndef GL_EYE_PLANE
#define GL_EYE_PLANE 0x2502
#endif
#ifndef GL_TEXTURE_GEN_S
#define GL_TEXTURE_GEN_S 0x0C60
#endif
#ifndef GL_TEXTURE_GEN_T
#define GL_TEXTURE_GEN_T 0x0C61
#endif
#ifndef GL_TEXTURE_GEN_R
#define GL_TEXTURE_GEN_R 0x0C62
#endif
#ifndef GL_TEXTURE_GEN_Q
#define GL_TEXTURE_GEN_Q 0x0C63
#endif
#ifndef GL_FOG_HINT
#define GL_FOG_HINT 0x0C54
#endif
#ifndef GL_FOG_DISTANCE_MODE_NV
#define GL_FOG_DISTANCE_MODE_NV 0x855A
#endif
#ifndef GL_EYE_RADIAL_NV
#define GL_EYE_RADIAL_NV 0x855B
#endif

typedef void *(*GLADloadproc)(const char *name);

static inline int gladLoadGLLoader(GLADloadproc)
{
    return 1;
}

static inline void glTexGeni(GLenum, GLenum, GLint)
{
}

static inline void glTexGenfv(GLenum, GLenum, const GLfloat *)
{
}

#endif
