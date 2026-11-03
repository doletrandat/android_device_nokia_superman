// SPDX-License-Identifier: Apache-2.0
// Fresh ES2 context only. Hidden, unqueued window + one internal texture at a
// time. These results never replace the original presentation pixel gate.
static uint64_t probe_ns() {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC,&t);
    return static_cast<uint64_t>(t.tv_sec)*1000000000ULL+t.tv_nsec;
}
static GLuint probe_shader(GLenum kind, const char* text) {
    GLuint s=glCreateShader(kind);glShaderSource(s,1,&text,nullptr);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    char log[256]{};GLsizei length=0;glGetShaderInfoLog(s,sizeof(log),&length,log);
    std::printf("NATIVE_SF_SHADER stage=%s compile=%d log=%s diagnostic_only=true\n",kind==GL_VERTEX_SHADER?"vertex":"fragment",ok,log);
    if(!ok || !check_gl("probe_shader_compile")) {glDeleteShader(s);return 0;}
    return s;
}
static void probe_samples(const char* path, const char* method, unsigned q,
                          int width, int height, const unsigned char* color,
                          GLint native_format, GLint native_type,
                          unsigned& matches, unsigned& errors, uint64_t& read_ns) {
    for(unsigned sample=0;sample<3;++sample) {
        const int x=width==1?0:(q%2)*(width/2)+width/8+sample*width/12;
        const int y=height==1?0:(q/2)*(height/2)+height/8+sample*height/16;
        unsigned char rgba[4]{};const auto begin=probe_ns();
        glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        const bool ok=check_gl("matrix_RGBA_read",-1,q,sample);read_ns+=probe_ns()-begin;
        const bool match=ok && !std::memcmp(rgba,color,4);matches+=match;errors+=!ok;
        std::printf("NATIVE_SF_MATRIX_PIXEL path=%s method=%s quadrant=%u sample=%u x=%d y=%d expected=%u/%u/%u/%u RGBA=%u/%u/%u/%u match=%d GL_ok=%d diagnostic_only=true\n",path,method,q,sample,x,y,color[0],color[1],color[2],color[3],rgba[0],rgba[1],rgba[2],rgba[3],match,ok);
        if(native_format==0x80e1 && native_type==GL_UNSIGNED_BYTE) {
            unsigned char raw[4]{};const auto native_begin=probe_ns();
            glReadPixels(x,y,1,1,0x80e1,GL_UNSIGNED_BYTE,raw);
            const bool native_ok=check_gl("matrix_BGRA_read",-1,q,sample);read_ns+=probe_ns()-native_begin;
            const unsigned char logical[4]={raw[2],raw[1],raw[0],raw[3]};errors+=!native_ok;
            std::printf("NATIVE_SF_MATRIX_NATIVE path=%s method=%s quadrant=%u sample=%u raw_BGRA=%u/%u/%u/%u logical_RGBA=%u/%u/%u/%u match=%d GL_ok=%d diagnostic_only=true\n",path,method,q,sample,raw[0],raw[1],raw[2],raw[3],logical[0],logical[1],logical[2],logical[3],native_ok && !std::memcmp(logical,color,4),native_ok);
        }
    }
}
static bool probe_matrix() {
    constexpr unsigned char colors[4][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
    const GLuint vs=probe_shader(GL_VERTEX_SHADER,"attribute vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}");
    const GLuint fs=probe_shader(GL_FRAGMENT_SHADER,"precision mediump float;uniform vec4 color;void main(){gl_FragColor=color;}");
    if(!vs || !fs) {glDeleteShader(vs);glDeleteShader(fs);return false;}
    const GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);
    glBindAttribLocation(program,0,"position");glLinkProgram(program);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
    char log[256]{};GLsizei length=0;glGetProgramInfoLog(program,sizeof(log),&length,log);
    std::printf("NATIVE_SF_SHADER stage=program link=%d log=%s diagnostic_only=true\n",linked,log);
    glDeleteShader(vs);glDeleteShader(fs);
    const GLint uniform=glGetUniformLocation(program,"color");
    if(!linked || uniform<0 || !check_gl("matrix_program")) {glDeleteProgram(program);return false;}
    const GLfloat vertices[]={-1,-1,1,-1,-1,1,1,1};
    glBindBuffer(GL_ARRAY_BUFFER,0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vertices);
    glEnableVertexAttribArray(0);
    const char* extensions=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const bool bgra_texture=extensions && std::strstr(extensions,"GL_EXT_texture_format_BGRA8888");
    std::printf("NATIVE_SF_MATRIX_BEGIN paths=window,RGBA1,RGBA720,BGRA720 methods=scissored-clear,shader colors=red,green,blue,white samples_per_color=3 BGRA_texture_extension=%d no_queue=true CPU_writes=false throughput_claim=false\n",bgra_texture);
    bool operations_ok=check_gl("matrix_setup");
    for(unsigned path=0;path<4;++path) {
        const char* name=path==0?"window-BGRA720":path==1?"internal-RGBA1":path==2?"internal-RGBA720":"internal-BGRA720";
        if(path==3 && !bgra_texture) {std::printf("NATIVE_SF_MATRIX_SKIP path=%s reason=extension_missing\n",name);continue;}
        const int width=path==1?1:720,height=path==1?1:1280;
        GLuint texture=0,framebuffer=0;
        if(path) {
            glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            const GLenum format=path==3?0x80e1:GL_RGBA;
            glTexImage2D(GL_TEXTURE_2D,0,format,width,height,0,format,GL_UNSIGNED_BYTE,nullptr);
            glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        } else glBindFramebuffer(GL_FRAMEBUFFER,0);
        const GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER);GLint format=0,type=0;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT,&format);glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE,&type);
        const bool setup=check_gl("matrix_target_setup");operations_ok&=setup && status==GL_FRAMEBUFFER_COMPLETE;
        std::printf("NATIVE_SF_MATRIX_TARGET path=%s size=%dx%d status=0x%x read_format=0x%x read_type=0x%x GL_ok=%d diagnostic_only=true\n",name,width,height,status,format,type,setup);
        if(setup && status==GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0,0,width,height);glEnable(GL_SCISSOR_TEST);
            for(unsigned method=0;method<2;++method) {
                const char* method_name=method?"shader":"scissored-clear";
                unsigned matches=0,errors=0;uint64_t read_ns=0;const auto start=probe_ns();
                glUseProgram(method?program:0);
                for(unsigned q=0;q<4;++q) {
                    glScissor(width==1?0:(q%2)*(width/2),height==1?0:(q/2)*(height/2),width==1?1:width/2,height==1?1:height/2);
                    const auto& c=colors[q];
                    if(method) {glUniform4f(uniform,c[0]/255.0f,c[1]/255.0f,c[2]/255.0f,1.0f);glDrawArrays(GL_TRIANGLE_STRIP,0,4);}
                    else {glClearColor(c[0]/255.0f,c[1]/255.0f,c[2]/255.0f,1.0f);glClear(GL_COLOR_BUFFER_BIT);}
                    const bool draw_ok=check_gl("matrix_render",-1,q);errors+=!draw_ok;
                    if(draw_ok) probe_samples(name,method_name,q,width,height,c,format,type,matches,errors,read_ns);
                }
                operations_ok&=errors==0;
                std::printf("NATIVE_SF_MATRIX_RESULT path=%s method=%s RGBA_matches=%u expected_samples=12 GL_errors=%u wall_ns=%" PRIu64 " readback_ns=%" PRIu64 " diagnostic_only=true no_presentation_pass=true throughput_claim=false\n",name,method_name,matches,errors,probe_ns()-start,read_ns);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER,0);glBindTexture(GL_TEXTURE_2D,0);
        if(framebuffer)glDeleteFramebuffers(1,&framebuffer);if(texture)glDeleteTextures(1,&texture);
        operations_ok&=check_gl("matrix_target_cleanup");
    }
    glUseProgram(0);glDisableVertexAttribArray(0);glDeleteProgram(program);
    glViewport(0,0,720,1280);glDisable(GL_SCISSOR_TEST);
    operations_ok&=check_gl("matrix_cleanup");
    std::printf("NATIVE_SF_MATRIX_END GL_operations_ok=%d original_pixel_gate_unchanged=true\n",operations_ok);
    return operations_ok;
}
