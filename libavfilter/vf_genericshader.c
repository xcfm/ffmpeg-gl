#include "libavutil/opt.h"
#include "internal.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <float.h>

#define PIXEL_FORMAT (GL_RGB)

static const float position[12] = {
  -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f};

static const GLchar *v_shader_source =
  "attribute vec2 position;\n"
  "varying vec2 _uv;\n"
  "void main(void) {\n"
  "  gl_Position = vec4(position, 0, 1);\n"
  "  vec2 uv = position * 0.5 + 0.5;\n"
  "  _uv = uv;\n"
  "}\n";

static const GLchar *f_shader_template =
  "uniform sampler2D to;\n"
  "varying vec2 _uv;\n"
  "uniform float progress;\n"
  "uniform vec2 resolution;"
  "\n"
  "\n%s\n"
  "\n"
  "void main() {\n"
  "  gl_FragColor = transition(_uv);\n"
  "}\n";

// 默认的 transition 实现，do noting
static const GLchar *f_default_transition_source =
  "vec4 transition (vec2 uv) {\n"
  "  return texture2D(to, uv);\n"
  "}\n";

typedef struct {
  const AVClass *class;

  // input options
  double duration;
  double offset;
  char *source;

  // timestamp of the first frame in the output, in the timebase units
  int64_t first_pts;

  // uniforms
  GLint         progress; // video 进度
  GLuint        frame_tex;  // f.glsl 中的 sampler2D tex

  // internal state
  GLuint        program;
  GLuint        pos_buf;

  GLFWwindow    *window;

  GLchar *f_shader_source; // 拼接后的 glsl
} GenericShaderContext;

#define OFFSET(x) offsetof(GenericShaderContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption genericshader_options[] = {
  { "duration", "transition duration in seconds", OFFSET(duration), AV_OPT_TYPE_DOUBLE, {.dbl=1.0}, 0, DBL_MAX, FLAGS },
  { "offset", "delay before startingtransition in seconds", OFFSET(offset), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, 0, DBL_MAX, FLAGS },
  { "source", "path to the gl-transition source file (defaults to basic fade)", OFFSET(source), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
  {NULL}
};

AVFILTER_DEFINE_CLASS(genericshader);

static GLuint build_shader(AVFilterContext *ctx, const GLchar *shader_source, GLenum type) {
  GLuint shader = glCreateShader(type);
  if (!shader || !glIsShader(shader)) {
    return 0;
  }

  glShaderSource(shader, 1, &shader_source, 0);
  glCompileShader(shader);

  GLint status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

  return status == GL_TRUE ? shader : 0;
}

static int build_program(AVFilterContext *ctx) {
  GLuint v_shader, f_shader;
  GenericShaderContext *gs = ctx->priv;

  if (!(v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER)))
        {
    av_log(ctx, AV_LOG_ERROR, "invalid vertex shader\n");
    return -1;
  }

  char *source = NULL;

  if (gs->source) {
    FILE *f = fopen(gs->source, "rb");

    if (!f) {
      av_log(ctx, AV_LOG_ERROR, "invalid glsl source file \"%s\"\n", gs->source);
      return -1;
    }

    fseek(f, 0, SEEK_END);
    unsigned long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    source = malloc(fsize + 1);
    fread(source, fsize, 1, f);
    fclose(f);

    source[fsize] = 0;
  }

  // 加载外部 glsl 文件
  const char *transition_source = source ? source : f_default_transition_source;

  int len = strlen(f_shader_template) + strlen(transition_source);
  gs->f_shader_source = av_calloc(len, sizeof(*gs->f_shader_source));
  if (!gs->f_shader_source) {
    return AVERROR(ENOMEM);
  }

  snprintf(gs->f_shader_source, len * sizeof(*gs->f_shader_source), f_shader_template, transition_source);

  if (source) {
    free(source);
    source = NULL;
  }

  if (!(f_shader = build_shader(ctx, gs->f_shader_source, GL_FRAGMENT_SHADER))) {
    av_log(ctx, AV_LOG_ERROR, "invalid fragment shader\n");
    return -1;
  }

  gs->program = glCreateProgram();
  glAttachShader(gs->program, v_shader);
  glAttachShader(gs->program, f_shader);
  glLinkProgram(gs->program);

  GLint status;
  glGetProgramiv(gs->program, GL_LINK_STATUS, &status);
  return status == GL_TRUE ? 0 : -1;
}

static void vbo_setup(GenericShaderContext *gs)
{
  glGenBuffers(1, &gs->pos_buf);
  glBindBuffer(GL_ARRAY_BUFFER, gs->pos_buf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);

  GLint loc = glGetAttribLocation(gs->program, "position");
  glEnableVertexAttribArray(loc);
  glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
}

static void tex_setup(AVFilterLink *inlink)
{
  AVFilterContext     *ctx = inlink->dst;
  GenericShaderContext *gs = ctx->priv;

  glGenTextures(1, &gs->frame_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gs->frame_tex);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, inlink->w, inlink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);

  glUniform1i(glGetUniformLocation(gs->program, "to"), 0);
}

static void uniforms_setup(AVFilterLink *fromLink)
{
  AVFilterContext     *ctx = fromLink->dst;
  GenericShaderContext *c  = ctx->priv;

  c->progress = glGetUniformLocation(c->program, "progress");
  glUniform1f(c->progress, 0.0f);

  av_log(NULL, AV_LOG_INFO, "------ width = %d, height = %d \n", fromLink->w, fromLink->h);
  glUniform2f(glGetUniformLocation(c->program, "resolution"), (float)fromLink->w, (float)fromLink->h);
}

static int config_props(AVFilterLink *inlink) {
  AVFilterContext     *ctx = inlink->dst;
  GenericShaderContext *gs = ctx->priv;

  glfwWindowHint(GLFW_VISIBLE, 0);
  gs->window = glfwCreateWindow(inlink->w, inlink->h, "", NULL, NULL);
  if (!gs->window) {
    av_log(ctx, AV_LOG_ERROR, "config_props setup_gl ERROR: ");
    return -1;
  }
  glfwMakeContextCurrent(gs->window);

  #ifndef __APPLE__
    glewExperimental = GL_TRUE;
    glewInit();
  #endif

  glViewport(0, 0, inlink->w, inlink->h);

  int ret;
  if((ret = build_program(ctx)) < 0) {
    return ret;
  }

  glUseProgram(gs->program);
  vbo_setup(gs);
  tex_setup(inlink);
  uniforms_setup(inlink);

  return 0;
}

// blend opengl & video
static int filter_frame(AVFilterLink *inlink, AVFrame *in) {
  AVFilterContext *ctx = inlink->dst;
  GenericShaderContext *c = ctx->priv;
  AVFilterLink *outlink = ctx->outputs[0];

  AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
  if (!out) {
    av_frame_free(&in);
    return AVERROR(ENOMEM);
  }

  av_frame_copy_props(out, in);

  glfwMakeContextCurrent(c->window);

  glUseProgram(c->program);

  const float ts = in->pts / (float)inlink->time_base.den - c->offset;
  const float progress = FFMAX(0.0f, FFMIN(1.0f, ts / c->duration));
  glUniform1f(c->progress, progress);
  // av_log(NULL, AV_LOG_INFO, "----apply_transition progress: %lld--====: %lld ======: %f 》》》》offset: %f ---- ----: %f\n", out->pts,  c->first_pts, (float)inlink->time_base.den, c->offset, ts);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, c->frame_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, inlink->w, inlink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, in->data[0]);

  glDrawArrays(GL_TRIANGLES, 0, 6);
  glReadPixels(0, 0, outlink->w, outlink->h, PIXEL_FORMAT, GL_UNSIGNED_BYTE, (GLvoid *)out->data[0]);

  av_frame_free(&in);
  return ff_filter_frame(outlink, out);
}

static av_cold int init(AVFilterContext *ctx)
{
  GenericShaderContext *c = ctx->priv;
  c->first_pts = AV_NOPTS_VALUE;

  if (!glfwInit())
  {
    return -1;
  }
  return 0;
}

static av_cold void uninit(AVFilterContext *ctx) {
  GenericShaderContext *gs = ctx->priv;

  if (gs->window) {
    glDeleteTextures(1, &gs->frame_tex);
    glDeleteProgram(gs->program);
    glDeleteBuffers(1, &gs->pos_buf);
    glfwDestroyWindow(gs->window);
  }

  if (gs->f_shader_source) {
    av_freep(&gs->f_shader_source);
  }
}

// query_formats 声明 filter 支持的格式
static int query_formats(AVFilterContext *ctx) {
  static const enum AVPixelFormat formats[] = {
    AV_PIX_FMT_RGB24,
    AV_PIX_FMT_NONE
  };

  return ff_set_common_formats(ctx, ff_make_format_list(formats));
}

static const AVFilterPad genericshader_inputs[] = {
  {
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = config_props,
    .filter_frame = filter_frame
    },
    {NULL}
  };

static const AVFilterPad genericshader_outputs[] = {
  {
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO
    },
    {NULL}
  };

AVFilter ff_vf_genericshader = {
  .name          = "genericshader",
  .description   = NULL_IF_CONFIG_SMALL("Generic OpenGL shader filter"),
  .priv_size     = sizeof(GenericShaderContext),
  .init          = init,
  .uninit        = uninit,
  .query_formats = query_formats,
  .inputs        = genericshader_inputs,
  .outputs       = genericshader_outputs,
  .priv_class    = &genericshader_class,
  .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC
};
