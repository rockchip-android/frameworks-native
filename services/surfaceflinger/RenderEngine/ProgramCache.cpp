/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <utils/String8.h>

#include "ProgramCache.h"
#include "Program.h"
#include "Description.h"

const char FRAG_SHADER[] = {
#include "frag_hdr2sdr.h"
};

const char VERT_SHADER[] = {
#include "vert_hdr2sdr.h"
};

#if defined(EECOLOR)

#include <dlfcn.h>

#include "eeColorAPI/eeColorAPI.h"
eeColor::APIFunctions gEEColorAPIFunctions;

#if ARCH_32
	#define EECOLOR_PATH "/system/lib/libeecolorapi.so"
#else
	#define EECOLOR_PATH "/system/lib64/libeecolorapi.so"
#endif

void eeColorCallback()
{
	__android_log_close();
	ALOGD("eeColorAPI eeColorCallback()");

	void* mDso = dlopen(EECOLOR_PATH, RTLD_NOW | RTLD_LOCAL);

	if (mDso == NULL)
	{
		ALOGE("eeColorAPI Callback can't not find /system/lib64/libeecolorapi.so ! error=%s \n",
					dlerror());
		return;

	}

	gEEColorAPIFunctions.pCompileShaders = (__CompileShaders)dlsym(mDso, eeColor::COMPILE_SHADERS_MANGLED);
	if (gEEColorAPIFunctions.pCompileShaders == NULL)
	{
		ALOGE("eeColorAPI cann't find pCompileShaders");
		dlclose(mDso);
		return;
	}


	gEEColorAPIFunctions.pReadTable = (__ReadTable)dlsym(mDso, eeColor::READ_TABLE_MANGLED);
	if (gEEColorAPIFunctions.pReadTable == NULL)
	{
		ALOGE("eeColorAPI cann't find pReadTable");
		dlclose(mDso);
		return;
	}

	gEEColorAPIFunctions.pEnableEEColor = (__EnableEEColor)dlsym(mDso, eeColor::ENABLE_EECOLOR_MANGLED);
	if (gEEColorAPIFunctions.pEnableEEColor == NULL)
	{
		ALOGE("eeColorAPI cann't find pEnableEEColor");
		dlclose(mDso);
		return;
	}

	gEEColorAPIFunctions.pIsEEColorEnabled = (__IsEEColorEnabled)dlsym(mDso, eeColor::IS_EECOLOR_ENABLED_MANGLED);
	if (gEEColorAPIFunctions.pIsEEColorEnabled == NULL)
	{
		ALOGE("eeColorAPI cann't find pIsEEColorEnabled");
		dlclose(mDso);
		return;
	}

	gEEColorAPIFunctions.pSetTableType = (__SetTableType)dlsym(mDso, eeColor::SET_TABLE_TYPE_MANGLED);
	if (gEEColorAPIFunctions.pSetTableType == NULL)
	{
		ALOGE("eeColorAPI cann't find pSetTableType");
		dlclose(mDso);
		return;
	}

	gEEColorAPIFunctions.pGetTableType = (__GetTableType)dlsym(mDso, eeColor::GET_TABLE_TYPE_MANGLED);
	if (gEEColorAPIFunctions.pGetTableType == NULL)
	{
		ALOGE("eeColorAPI cann't find pGetTableType");
		dlclose(mDso);
		return;
	}

	gEEColorAPIFunctions.pUseProgram = (__UseProgram)dlsym(mDso, eeColor::USE_PROGRAM_MANGLED);
	if (gEEColorAPIFunctions.pUseProgram == NULL)
	{
		ALOGE("eeColorAPI cann't find pUseProgram");
		dlclose(mDso);
		return;
	}

	gEEColorAPIFunctions.pSetUniforms = (__SetUniforms)dlsym(mDso, eeColor::SET_UNIFORMS_MANGLED);
	if (gEEColorAPIFunctions.pSetUniforms == NULL)
	{
		ALOGE("eeColorAPI cann't find pSetUniforms");
		dlclose(mDso);
		return;
	}

	ALOGD("eeColorAPI Open success");
}

#endif // EECOLOR

namespace android {
// -----------------------------------------------------------------------------------------------


/*
 * A simple formatter class to automatically add the endl and
 * manage the indentation.
 */

class Formatter;
static Formatter& indent(Formatter& f);
static Formatter& dedent(Formatter& f);

class Formatter {
    String8 mString;
    int mIndent;
    typedef Formatter& (*FormaterManipFunc)(Formatter&);
    friend Formatter& indent(Formatter& f);
    friend Formatter& dedent(Formatter& f);
public:
    Formatter() : mIndent(0) {}

    String8 getString() const {
        return mString;
    }

    friend Formatter& operator << (Formatter& out, const char* in) {
        for (int i=0 ; i<out.mIndent ; i++) {
            out.mString.append("    ");
        }
        out.mString.append(in);
        out.mString.append("\n");
        return out;
    }
    friend inline Formatter& operator << (Formatter& out, const String8& in) {
        return operator << (out, in.string());
    }
    friend inline Formatter& operator<<(Formatter& to, FormaterManipFunc func) {
        return (*func)(to);
    }
};
Formatter& indent(Formatter& f) {
    f.mIndent++;
    return f;
}
Formatter& dedent(Formatter& f) {
    f.mIndent--;
    return f;
}

// -----------------------------------------------------------------------------------------------

ANDROID_SINGLETON_STATIC_INSTANCE(ProgramCache)

ProgramCache::ProgramCache() {
    // Until surfaceflinger has a dependable blob cache on the filesystem,
    // generate shaders on initialization so as to avoid jank.
    primeCache();
}

ProgramCache::~ProgramCache() {
}

void ProgramCache::primeCache() {
#if defined(EECOLOR)
	static bool bDoneOnce = false;
	if (!bDoneOnce)
	{
		bDoneOnce = true;

		eeColorCallback();

		__android_log_close();
		//ALOGD("eeColorAPI pCompileShaders");
		gEEColorAPIFunctions.pCompileShaders();
	}
#endif

    uint32_t shaderCount = 0;
    uint32_t keyMask = Key::BLEND_MASK | Key::OPACITY_MASK |
                       Key::PLANE_ALPHA_MASK | Key::TEXTURE_MASK;
    // Prime the cache for all combinations of the above masks,
    // leaving off the experimental color matrix mask options.

    nsecs_t timeBefore = systemTime();
    for (uint32_t keyVal = 0; keyVal <= keyMask; keyVal++) {
        Key shaderKey;
        shaderKey.set(keyMask, keyVal);
        uint32_t tex = shaderKey.getTextureTarget();
        if (tex != Key::TEXTURE_OFF &&
            tex != Key::TEXTURE_EXT &&
            tex != Key::TEXTURE_2D) {
            continue;
        }
        Program* program = mCache.valueFor(shaderKey);
        if (program == NULL) {
            program = generateProgram(shaderKey);
            mCache.add(shaderKey, program);
            shaderCount++;
        }
    }
    nsecs_t timeAfter = systemTime();
    float compileTimeMs = static_cast<float>(timeAfter - timeBefore) / 1.0E6;
    ALOGD("shader cache generated - %u shaders in %f ms\n", shaderCount, compileTimeMs);
}

ProgramCache::Key ProgramCache::computeKey(const Description& description) {
    Key needs;
    needs.set(Key::TEXTURE_MASK,
            !description.mTextureEnabled ? Key::TEXTURE_OFF :
            description.mTexture.getTextureTarget() == GL_TEXTURE_EXTERNAL_OES ? Key::TEXTURE_EXT :
            description.mTexture.getTextureTarget() == GL_TEXTURE_2D           ? Key::TEXTURE_2D :
            Key::TEXTURE_OFF)
    .set(Key::PLANE_ALPHA_MASK,
            (description.mPlaneAlpha < 1) ? Key::PLANE_ALPHA_LT_ONE : Key::PLANE_ALPHA_EQ_ONE)
    .set(Key::BLEND_MASK,
            description.mPremultipliedAlpha ? Key::BLEND_PREMULT : Key::BLEND_NORMAL)
    .set(Key::OPACITY_MASK,
            description.mOpaque ? Key::OPACITY_OPAQUE : Key::OPACITY_TRANSLUCENT)
    .set(Key::COLOR_MATRIX_MASK,
            description.mColorMatrixEnabled ? Key::COLOR_MATRIX_ON :  Key::COLOR_MATRIX_OFF)
			.set(Key::HDR_MASK,
            description.mHdr ? Key::HDR_ON :  Key::HDR_OFF);
    return needs;
}

String8 ProgramCache::generateVertexShader(const Key& needs) {
    Formatter vs;

    if(needs.hasHdr()){
        return String8(VERT_SHADER);
    }

    if (needs.isTexturing()) {
        vs  << "attribute vec4 texCoords;"
            << "varying vec2 outTexCoords;";
    }
    vs << "attribute vec4 position;"
       << "uniform mat4 projection;"
       << "uniform mat4 texture;"
       << "void main(void) {" << indent
       << "gl_Position = projection * position;";
    if (needs.isTexturing()) {
        vs << "outTexCoords = (texture * texCoords).st;";
    }
    vs << dedent << "}";

    return vs.getString();
}

String8 ProgramCache::generateFragmentShader(const Key& needs) {
    Formatter fs;

    if(needs.hasHdr()){
        return String8(FRAG_SHADER);
    }

    if (needs.getTextureTarget() == Key::TEXTURE_EXT) {
        fs << "#extension GL_OES_EGL_image_external : require";
    }

    // default precision is required-ish in fragment shaders
    fs << "precision mediump float;";

    if (needs.getTextureTarget() == Key::TEXTURE_EXT) {
        fs << "uniform samplerExternalOES sampler;"
           << "varying vec2 outTexCoords;";
    } else if (needs.getTextureTarget() == Key::TEXTURE_2D) {
        fs << "uniform sampler2D sampler;"
           << "varying vec2 outTexCoords;";
    } else if (needs.getTextureTarget() == Key::TEXTURE_OFF) {
        fs << "uniform vec4 color;";
    }
    if (needs.hasPlaneAlpha()) {
        fs << "uniform float alphaPlane;";
    }
    if (needs.hasColorMatrix()) {
        fs << "uniform mat4 colorMatrix;";
    }
    fs << "void main(void) {" << indent;
    if (needs.isTexturing()) {
        fs << "gl_FragColor = texture2D(sampler, outTexCoords);";
    } else {
        fs << "gl_FragColor = color;";
    }
    if (needs.isOpaque()) {
        fs << "gl_FragColor.a = 1.0;";
    }
    if (needs.hasPlaneAlpha()) {
        // modulate the alpha value with planeAlpha
        if (needs.isPremultiplied()) {
            // ... and the color too if we're premultiplied
            fs << "gl_FragColor *= alphaPlane;";
        } else {
            fs << "gl_FragColor.a *= alphaPlane;";
        }
    }

    if (needs.hasColorMatrix()) {
        if (!needs.isOpaque() && needs.isPremultiplied()) {
            // un-premultiply if needed before linearization
            fs << "gl_FragColor.rgb = gl_FragColor.rgb/gl_FragColor.a;";
        }
        fs << "vec4 transformed = colorMatrix * vec4(gl_FragColor.rgb, 1);";
        fs << "gl_FragColor.rgb = transformed.rgb/transformed.a;";
        if (!needs.isOpaque() && needs.isPremultiplied()) {
            // and re-premultiply if needed after gamma correction
            fs << "gl_FragColor.rgb = gl_FragColor.rgb*gl_FragColor.a;";
        }
    }

    fs << dedent << "}";

    return fs.getString();
}

Program* ProgramCache::generateProgram(const Key& needs) {
    // vertex shader
    String8 vs = generateVertexShader(needs);

    // fragment shader
    String8 fs = generateFragmentShader(needs);

    Program* program = new Program(needs, vs.string(), fs.string());
    return program;
}

void ProgramCache::useProgram(const Description& description) {

    // generate the key for the shader based on the description
    Key needs(computeKey(description));

     // look-up the program in the cache
    Program* program = mCache.valueFor(needs);
    if (program == NULL) {
        // we didn't find our program, so generate one...
        nsecs_t time = -systemTime();
        program = generateProgram(needs);
        mCache.add(needs, program);
        time += systemTime();

        //ALOGD(">>> generated new program: needs=%08X, time=%u ms (%d programs)",
        //        needs.mNeeds, uint32_t(ns2ms(time)), mCache.size());
    }

    // here we have a suitable program for this description
    if (program->isValid()) {
        program->use();
        program->setUniforms(description);
    }
}


} /* namespace android */
