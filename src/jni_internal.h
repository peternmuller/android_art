// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "indirect_reference_table.h"
#include "macros.h"
#include "reference_table.h"

#include <map>
#include <string>

namespace art {

class ClassLoader;
class Mutex;
class Runtime;
class SharedLibrary;
class Thread;

struct JavaVMExt : public JavaVM {
  JavaVMExt(Runtime* runtime, bool check_jni, bool verbose_jni);
  ~JavaVMExt();

  /*
   * Load native code from the specified absolute pathname.  Per the spec,
   * if we've already loaded a library with the specified pathname, we
   * return without doing anything.
   *
   * TODO: for better results we should canonicalize the pathname.  For fully
   * correct results we should stat to get the inode and compare that.  The
   * existing implementation is fine so long as everybody is using
   * System.loadLibrary.
   *
   * The library will be associated with the specified class loader.  The JNI
   * spec says we can't load the same library into more than one class loader.
   *
   * Returns true on success. On failure, returns false and sets *detail to a
   * human-readable description of the error or NULL if no detail is
   * available; ownership of the string is transferred to the caller.
   */
  bool LoadNativeLibrary(const std::string& path, ClassLoader* class_loader, char** detail);

  Runtime* runtime;

  bool check_jni;
  bool verbose_jni;

  // Used to hold references to pinned primitive arrays.
  ReferenceTable pin_table;

  // JNI global references.
  Mutex* globals_lock;
  IndirectReferenceTable globals;

  // JNI weak global references.
  Mutex* weak_globals_lock;
  IndirectReferenceTable weak_globals;

  std::map<std::string, SharedLibrary*> libraries;
};

struct JNIEnvExt : public JNIEnv {
  JNIEnvExt(Thread* self, bool check_jni);

  Thread* self;

  bool check_jni;

  // Are we in a "critical" JNI call?
  bool critical;

  // Entered JNI monitors, for bulk exit on thread detach.
  ReferenceTable  monitors;

  // JNI local references.
  IndirectReferenceTable locals;
};

}  // namespace art

#endif  // ART_SRC_JNI_INTERNAL_H_
