
#include <jni.h>

#include <lua.h>
#include <lauxlib.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include <LuaNScriptValue.h>

#include "engine.h"

// this one is really complicated
// instead of using the standard registry, we use our own table (so we can index things with numbers)
// we do a dual association when registering new functions, after attempting to look the function up
// we then pass the id to the engine value, which can be later looked up to call the function

// this also pops a value, and expects a function (can be cfunction or lua)

void engine_handleregistry(JNIEnv* env, engine_inst* inst, lua_State* state, engine_value* v) {
    v->type = ENGINE_LUA_FUNCTION;

    if (!lua_isfunction(state, -1) && !lua_iscfunction(state, -1)) {
        fprintf(stderr, "FATAL: engine_handleregistry(...) called without function on top of stack");
        fflush(stderr);
        abort();
    }
    
    // make copy of function, and put it on the top of the stack
    lua_pushvalue(state, -1);
    // push registry on the stack
    lua_getglobal(state, FUNCTION_REGISTRY);
    // if the registry doesn't exist, create a new one
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1); // copy
        lua_setglobal(state, FUNCTION_REGISTRY);
    }
    // swap registry and lua function, so that the function is on top
    engine_swap(state, -1, -2);
    // swap function (top) with value from function tablelua function as index
    // (result should be nil or a number)
    lua_gettable(state, -2);
    // stack:
    //  -1: nil or function index
    //  -2: __function table
    //  -3: copy of original function
    if (lua_isnil(state, -1)) { // no function mapped
        // pop the nil valuexs
        lua_pop(state, 1);
        // (we are now back to then func registry (-1) and then the original function (-2) on top)
        // let's swap that
        engine_swap(state, -1, -2);
        // increment and push new function index
        function_index++;
        lua_pushinteger(state, function_index);
        // swap top (function) with key under it (-2)
        engine_swap(state, -1, -2);
        // push association (function, id)
        lua_rawset(state, -3);
        // pop registry
        lua_pop(state, -1);
        
        v->data.func = function_index;
    }
    else { // if there is a function mapped already
        
        v->data.func = (uint32_t) lua_tonumber(state, -1);
        // corrupt mappings
        if (!(v->data.func)) {
            v->type = ENGINE_NULL;
        }
        // pop id, registry, and function under the registry
        lua_pop(state, 3);
    }
}

void engine_pushobject(JNIEnv* env, engine_inst* inst, lua_State* state, jobject obj) {
    // allocate new userdata (managed by lua)
    engine_userdata* userdata = lua_newuserdata(state, sizeof(engine_userdata));
    // create new global ref
    userdata->obj = (*env)->NewGlobalRef(env, obj);
    userdata->engine = inst;
    userdata->released = 0;
    // get our special metatable
    luaL_getmetatable(state, ENGINE_USERDATA_TYPE);
    // set metatable to our userdatum
    // it pops itself off the stack and assigns itself to index -2
    lua_setmetatable(state, -2);
}

// boring mapping
engine_value* engine_popvalue(JNIEnv* env, engine_inst* inst, lua_State* state) {
    
    ASSERTEX(env);
    
    engine_value* v = engine_newvalue(env, inst);
    if (lua_isnumber(state, -1)) {
        v->type = ENGINE_FLOATING;
        v->data.d = (double) lua_tonumber(state, -1);
        lua_pop(state, 1);
    }
    else if (lua_isboolean(state, -1)) {
        v->type = ENGINE_BOOLEAN;
        v->data.i = (long) lua_toboolean(state, -1);
        lua_pop(state, 1);
    }
    else if (lua_isstring(state, -1)) {
        v->type = ENGINE_STRING;

        /* Sublte differences in string handling */
#if LUA_VERSION_NUM < 502
        size_t len = lua_strlen(state, -1);
        const char* lstring = lua_tostring(state, -1);
#else
        size_t len;
        const char* lstring = lua_tolstring(state, -1, &len);
#endif
        char* s = malloc(sizeof(char) * (len + 1));
        s[len] = '\0';

        if (len > 0) {
            memcpy(s, lstring, len);
            // this is a (bad) temporary fix if lua tries to pass null characters to C
            // I can't think of a case where I would actually need to recieve null characters
            // on the java size, though, so I'm leaving it like this.
            size_t t;
            for (t = 0; t < len - 1; t++) {
                if (s[t] == '\0')
                    s[t] = '?';
            }
        }
        
        v->data.str = s;
        lua_pop(state, 1);
    }
    else if (lua_isnoneornil(state, -1)) {
        lua_pop(state, 1);
    }
    else if (lua_islightuserdata(state, -1)) {
        // this is a stub (we don't use lightuserdata)
        lua_pop(state, 1);
    }
    else if (lua_isuserdata(state, -1)) {
        v->type = ENGINE_JAVA_OBJECT;
        // get userdata
        engine_userdata* d = (engine_userdata*) luaL_checkudata(state, -1, ENGINE_USERDATA_TYPE);
        v->data.obj = (*env)->NewGlobalRef(env, d->obj);
        // pop userdata
        lua_pop(state, 1);
    }
    else if (lua_isfunction(state, -1)) {
        engine_handleregistry(env, inst, state, v);
    }
    // we don't need to return function pointers back to Java, they are
    // either utility functions from LuaN or LuaJIT, or closures that
    // wrap java functions.
    else if (lua_iscfunction(state, -1)) {
        lua_pop(state, 1);
    }
    // threads should _not_ be happening
    // if we run into this, scream at stderr and return null
    else if (lua_isthread(state, -1)) {
        lua_pop(state, 1);
    }
    
    // standard behaviour from the script layer is to convert into an array
    else if (lua_istable(state, -1)) {
        
        // if this happens, that means there was like a dozen tables nested in each other.
        // return null if the user is being retarded
        if (lua_gettop(state) >= 32) {
            if (engine_debug) {
                printf("C: lua API stack too large! (%d)", lua_gettop(state));
            }
            lua_pop(state, 1);
            return v;
        }
        
        v->type = ENGINE_ARRAY;
        // we're effectively calculating the table length like the # operator does
        unsigned short idx = 1;
        while (1) {
            if (idx == USHRT_MAX - 1) break;
            lua_pushinteger(state, (int) idx);
            lua_rawget(state, -2);
            if (lua_isnil(state, -1)) {
                lua_pop(state, 1);
                break;
            }
            lua_pop(state, 1);
            idx++;
        }
        --idx; /* subtract once, since the last value had to be nil to break */
        v->data.array.length = idx;
        v->data.array.values = idx ? malloc(sizeof(engine_value*) * idx) : NULL;
        if (engine_debug) {
            printf("C: passing lua table of size %lu", (unsigned long) v->data.array.length);
        }
        unsigned short i;
        for (i = 1; i <= idx; i++) {
            // push key
            lua_pushinteger(state, i);
            // swap key with value (of some sort)
            lua_rawget(state, -2);
            // recurrrrrssssiiiiioooon (and popping the value)
            v->data.array.values[i - 1] = engine_popvalue(env, inst, state);
        }
        lua_pop(state, 1);
    }
    return v;
}

void engine_pushvalue(JNIEnv* env, engine_inst* inst, lua_State* state, engine_value* value) {
    
    ASSERTEX(env);
    
    if (value->type == ENGINE_BOOLEAN) {
        lua_pushboolean(state, (int) value->data.i);
    }
    else if (value->type == ENGINE_FLOATING) {
        lua_pushnumber(state, value->data.d);
    }
    else if (value->type == ENGINE_INTEGRAL) {
        lua_pushnumber(state, (double) value->data.i);
    }
    else if (value->type == ENGINE_STRING) {
        lua_pushstring(state, value->data.str);
    }
    else if (value->type == ENGINE_ARRAY) {
        
        // overflow (well, not really, but it shouldn't be getting this big)
        if (lua_gettop(state) >= 32) {
            if (engine_debug) {
                printf("C: lua API stack too large! (%d)", lua_gettop(state));
            }
            lua_pushnil(state);
            return;
        }
        
        // create and push new table
        lua_newtable(state);
        unsigned short i;
        for (i = 0; i < value->data.array.length; ++i) {
            // push key
            lua_pushinteger(state, i + 1);
            // push value
            if (value->data.array.values[i]) {
                engine_pushvalue(env, inst, state, value->data.array.values[i]);
            }
            else {
                lua_pushnil(state);
            }
            // set table entry
            lua_rawset(state, -3);
        }
    }
    else if (value->type == ENGINE_JAVA_OBJECT) {
        engine_pushobject(env, inst, state, value->data.obj);
    }
    else if (value->type == ENGINE_LUA_GLOBALS) {
        // stub, just use _G instead.
        lua_pushnil(state);
    }
    else if (value->type == ENGINE_LUA_FUNCTION) {
        // stub, could lookup into the registry but it makes no sense to send a function back to lua
        lua_pushnil(state);
    }
    else if (value->type == ENGINE_JAVA_LAMBDA_FUNCTION) {
        // magic
        engine_pushlambda(env, inst, value->data.lfunc.lambda, value->data.lfunc.class_array);
    }
    else if (value->type == ENGINE_JAVA_REFLECT_FUNCTION) {
        // you're a wizard, harry
        engine_pushreflect(env, inst, value->data.rfunc.reflect_method, value->data.rfunc.obj_inst);
    }
    else if (value->type == ENGINE_NULL) {
        lua_pushnil(state);
    }
}

/*
 * Class:     ca_jarcode_consoles_computer_interpreter_luanative_LuaNFunctionFactory
 * Method:    createFunction
 * Signature: ([Ljava/lang/Class;Ljava/lang/Object;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptFunction;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNFunctionFactory_createFunction___3Ljava_lang_Class_2Ljava_lang_Object_2
(JNIEnv* env, jobject this, jobjectArray class_array, jobject lambda) {
    engine_value* value = engine_newsharedvalue(env);
    value->type = ENGINE_JAVA_LAMBDA_FUNCTION;
    value->data.lfunc.class_array = (*env)->NewGlobalRef(env, class_array);
    value->data.lfunc.lambda = (*env)->NewGlobalRef(env, lambda);
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_consoles_computer_interpreter_luanative_LuaNFunctionFactory
 * Method:    createFunction
 * Signature: (Ljava/lang/reflect/Method;Ljava/lang/Object;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptFunction;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNFunctionFactory_createFunction__Ljava_lang_reflect_Method_2Ljava_lang_Object_2
(JNIEnv* env, jobject this, jobject reflect_method, jobject obj_inst) {
    engine_value* value = engine_newsharedvalue(env);
    value->type = ENGINE_JAVA_REFLECT_FUNCTION;
    value->data.rfunc.reflect_method = (*env)->NewGlobalRef(env, reflect_method);
    value->data.rfunc.obj_inst = (*env)->NewGlobalRef(env, obj_inst);
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_consoles_computer_interpreter_luanative_LuaNValueFactory
 * Method:    translate
 * Signature: (ZLca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translate__ZLca_jarcode_ascript_interfaces_ScriptValue_2
(JNIEnv* env, jobject this, jboolean boolean, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_BOOLEAN;
    value->data.i = boolean;
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    translate
 * Signature: (FLca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translate__FLca_jarcode_ascript_interfaces_ScriptValue_2
(JNIEnv* env, jobject this, jfloat f, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_FLOATING;
    value->data.d = (double) f;
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    translate
 * Signature: (DLca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translate__DLca_jarcode_ascript_interfaces_ScriptValue_2
(JNIEnv* env, jobject this, jdouble d, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_FLOATING;
    value->data.d = d;
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    translate
 * Signature: (Ljava/lang/String;Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translate__Ljava_lang_String_2Lca_jarcode_ascript_interfaces_ScriptValue_2
(JNIEnv* env, jobject this, jstring str, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    const char* characters = (*env)->GetStringUTFChars(env, str, 0);
    value->type = ENGINE_STRING;
    size_t len = strlen(characters);
    value->data.str = malloc(sizeof(char) * (len + 1));
    value->data.str[len] = '\0';
    if (len > 0)
        memmove(value->data.str, characters, len);
    (*env)->ReleaseStringUTFChars(env, str, characters);
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    translate
 * Signature: (ILca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translate__ILca_jarcode_ascript_interfaces_ScriptValue_2
(JNIEnv* env, jobject this, jint i, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_INTEGRAL;
    value->data.i = i;
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    translate
 * Signature: (JLca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translate__JLca_jarcode_ascript_interfaces_ScriptValue_2
(JNIEnv* env, jobject this, jlong l, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_INTEGRAL;
    value->data.i = l;
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    translate
 * Signature: (SLca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translate__SLca_jarcode_ascript_interfaces_ScriptValue_2
(JNIEnv* env, jobject this, jshort s, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_INTEGRAL;
    value->data.i = s;
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    translate
 * Signature: (BLca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translate__BLca_jarcode_ascript_interfaces_ScriptValue_2
(JNIEnv* env, jobject this, jbyte b, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_INTEGRAL;
    value->data.i = b;
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    list
 * Signature: ([Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_list
(JNIEnv* env, jobject this, jobjectArray elements, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_ARRAY;
    jsize len = (*env)->GetArrayLength(env, elements);
    value->data.array.values = len ? malloc(sizeof(engine_inst*) * len) : 0;
    value->data.array.length = (size_t) len;
    if (engine_debug) {
        printf("C: creating engine array from Java array, size: %lu\n", (unsigned long) len);
    }
    int t;
    for (t = 0; t < len; t++) {
        jobject element = (*env)->GetObjectArrayElement(env, elements, t);
        engine_value* element_value = engine_unwrap(env, element);
        if (element_value) {
            value->data.array.values[t] = value_copy(env, element_value);
        }
        else value->data.array.values[t] = 0;
    }
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    nullValue
 * Signature: (Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_nullValue
(JNIEnv* env, jobject this, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    return engine_wrap(env, value);
}

/*
 * Class:     ca_jarcode_ascript_luanative_LuaNValueFactory
 * Method:    translateObj
 * Signature: (Ljava/lang/Object;Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;)Lca/jarcode/consoles/computer/interpreter/interfaces/ScriptValue;
 */
JNIEXPORT jobject JNICALL Java_ca_jarcode_ascript_luanative_LuaNValueFactory_translateObj
(JNIEnv* env, jobject this, jobject obj, jobject jglobals) {
    if (!jglobals) {
        throw(env, "tried to translate with null globals");
        return 0;
    }
    engine_value* globals = engine_unwrap(env, jglobals);
    engine_inst* inst = globals->inst;
    engine_value* value = engine_newvalue(env, inst);
    value->type = ENGINE_JAVA_OBJECT;
    value->data.obj = (*env)->NewGlobalRef(env, obj);
    return engine_wrap(env, value);
}
