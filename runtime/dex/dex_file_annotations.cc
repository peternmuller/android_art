/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "dex_file_annotations.h"

#include <stdlib.h>

#include "android-base/macros.h"
#include "android-base/stringprintf.h"
#include "art_field-inl.h"
#include "art_method-alloc-inl.h"
#include "base/sdk_version.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_instruction-inl.h"
#include "jni/jni_internal.h"
#include "jvalue-inl.h"
#include "mirror/array-alloc-inl.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/field.h"
#include "mirror/method.h"
#include "mirror/object_array-alloc-inl.h"
#include "mirror/object_array-inl.h"
#include "oat/oat_file.h"
#include "obj_ptr-inl.h"
#include "reflection.h"
#include "thread.h"
#include "well_known_classes.h"

namespace art HIDDEN {

using android::base::StringPrintf;

using dex::AnnotationItem;
using dex::AnnotationSetItem;
using dex::AnnotationSetRefItem;
using dex::AnnotationSetRefList;
using dex::AnnotationsDirectoryItem;
using dex::FieldAnnotationsItem;
using dex::MethodAnnotationsItem;
using dex::ParameterAnnotationsItem;

struct DexFile::AnnotationValue {
  JValue value_;
  uint8_t type_;
};

namespace {

// A helper class that contains all the data needed to do annotation lookup.
class ClassData {
 public:
  explicit ClassData(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_)
    : ClassData(ScopedNullHandle<mirror::Class>(),  // klass
                method,
                *method->GetDexFile(),
                &method->GetClassDef()) {}

  // Requires Scope to be able to create at least 1 handles.
  template <typename Scope>
  ClassData(Scope& hs, ArtField* field) REQUIRES_SHARED(Locks::mutator_lock_)
    : ClassData(hs.NewHandle(field->GetDeclaringClass())) { }

  explicit ClassData(Handle<mirror::Class> klass) REQUIRES_SHARED(art::Locks::mutator_lock_)
    : ClassData(klass,  // klass
                nullptr,  // method
                klass->GetDexFile(),
                klass->GetClassDef()) {}

  const DexFile& GetDexFile() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return dex_file_;
  }

  const dex::ClassDef* GetClassDef() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return class_def_;
  }

  ObjPtr<mirror::DexCache> GetDexCache() const REQUIRES_SHARED(Locks::mutator_lock_) {
    if (method_ != nullptr) {
      return method_->GetDexCache();
    } else {
      return real_klass_->GetDexCache();
    }
  }

  ObjPtr<mirror::ClassLoader> GetClassLoader() const REQUIRES_SHARED(Locks::mutator_lock_) {
    if (method_ != nullptr) {
      return method_->GetDeclaringClass()->GetClassLoader();
    } else {
      return real_klass_->GetClassLoader();
    }
  }

  ObjPtr<mirror::Class> GetRealClass() const REQUIRES_SHARED(Locks::mutator_lock_) {
    if (method_ != nullptr) {
      return method_->GetDeclaringClass();
    } else {
      return real_klass_.Get();
    }
  }

 private:
  ClassData(Handle<mirror::Class> klass,
            ArtMethod* method,
            const DexFile& dex_file,
            const dex::ClassDef* class_def) REQUIRES_SHARED(Locks::mutator_lock_)
      : real_klass_(klass),
        method_(method),
        dex_file_(dex_file),
        class_def_(class_def) {
    DCHECK((method_ == nullptr) || real_klass_.IsNull());
  }

  Handle<mirror::Class> real_klass_;
  ArtMethod* method_;
  const DexFile& dex_file_;
  const dex::ClassDef* class_def_;

  DISALLOW_COPY_AND_ASSIGN(ClassData);
};

ObjPtr<mirror::Object> CreateAnnotationMember(const ClassData& klass,
                                              Handle<mirror::Class> annotation_class,
                                              const uint8_t** annotation)
    REQUIRES_SHARED(Locks::mutator_lock_);

bool IsVisibilityCompatible(uint32_t actual, uint32_t expected) {
  if (expected == DexFile::kDexVisibilityRuntime) {
    if (IsSdkVersionSetAndAtMost(Runtime::Current()->GetTargetSdkVersion(), SdkVersion::kM)) {
      return actual == DexFile::kDexVisibilityRuntime || actual == DexFile::kDexVisibilityBuild;
    }
  }
  return actual == expected;
}

static const AnnotationSetItem* FindAnnotationSetForField(const DexFile& dex_file,
                                                          const dex::ClassDef& class_def,
                                                          uint32_t field_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const AnnotationsDirectoryItem* annotations_dir = dex_file.GetAnnotationsDirectory(class_def);
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  const FieldAnnotationsItem* field_annotations = dex_file.GetFieldAnnotations(annotations_dir);
  if (field_annotations == nullptr) {
    return nullptr;
  }
  uint32_t field_count = annotations_dir->fields_size_;
  for (uint32_t i = 0; i < field_count; ++i) {
    if (field_annotations[i].field_idx_ == field_index) {
      return dex_file.GetFieldAnnotationSetItem(field_annotations[i]);
    }
  }
  return nullptr;
}

static const AnnotationSetItem* FindAnnotationSetForField(ArtField* field)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Class> klass = field->GetDeclaringClass();
  const dex::ClassDef* class_def = klass->GetClassDef();
  if (class_def == nullptr) {
    DCHECK(klass->IsProxyClass());
    return nullptr;
  }
  return FindAnnotationSetForField(*field->GetDexFile(), *class_def, field->GetDexFieldIndex());
}

const AnnotationItem* SearchAnnotationSet(const DexFile& dex_file,
                                          const AnnotationSetItem* annotation_set,
                                          const char* descriptor,
                                          uint32_t visibility)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const AnnotationItem* result = nullptr;
  for (uint32_t i = 0; i < annotation_set->size_; ++i) {
    const AnnotationItem* annotation_item = dex_file.GetAnnotationItem(annotation_set, i);
    if (!IsVisibilityCompatible(annotation_item->visibility_, visibility)) {
      continue;
    }
    const uint8_t* annotation = annotation_item->annotation_;
    uint32_t type_index = DecodeUnsignedLeb128(&annotation);

    if (strcmp(descriptor, dex_file.GetTypeDescriptor(dex::TypeIndex(type_index))) == 0) {
      result = annotation_item;
      break;
    }
  }
  return result;
}

inline static void SkipEncodedValueHeaderByte(const uint8_t** annotation_ptr) {
  (*annotation_ptr)++;
}

bool SkipAnnotationValue(const DexFile& dex_file, const uint8_t** annotation_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint8_t* annotation = *annotation_ptr;
  uint8_t header_byte = *(annotation++);
  uint8_t value_type = header_byte & DexFile::kDexAnnotationValueTypeMask;
  uint8_t value_arg = header_byte >> DexFile::kDexAnnotationValueArgShift;
  int32_t width = value_arg + 1;

  switch (value_type) {
    case DexFile::kDexAnnotationByte:
    case DexFile::kDexAnnotationShort:
    case DexFile::kDexAnnotationChar:
    case DexFile::kDexAnnotationInt:
    case DexFile::kDexAnnotationLong:
    case DexFile::kDexAnnotationFloat:
    case DexFile::kDexAnnotationDouble:
    case DexFile::kDexAnnotationString:
    case DexFile::kDexAnnotationType:
    case DexFile::kDexAnnotationMethod:
    case DexFile::kDexAnnotationField:
    case DexFile::kDexAnnotationEnum:
      break;
    case DexFile::kDexAnnotationArray:
    {
      uint32_t size = DecodeUnsignedLeb128(&annotation);
      for (; size != 0u; --size) {
        if (!SkipAnnotationValue(dex_file, &annotation)) {
          return false;
        }
      }
      width = 0;
      break;
    }
    case DexFile::kDexAnnotationAnnotation:
    {
      DecodeUnsignedLeb128(&annotation);  // unused type_index
      uint32_t size = DecodeUnsignedLeb128(&annotation);
      for (; size != 0u; --size) {
        DecodeUnsignedLeb128(&annotation);  // unused element_name_index
        if (!SkipAnnotationValue(dex_file, &annotation)) {
          return false;
        }
      }
      width = 0;
      break;
    }
    case DexFile::kDexAnnotationBoolean:
    case DexFile::kDexAnnotationNull:
      width = 0;
      break;
    default:
      LOG(FATAL) << StringPrintf("Bad annotation element value byte 0x%02x", value_type);
      UNREACHABLE();
  }

  annotation += width;
  *annotation_ptr = annotation;
  return true;
}

const uint8_t* SearchEncodedAnnotation(const DexFile& dex_file,
                                       const uint8_t* annotation,
                                       const char* name)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DecodeUnsignedLeb128(&annotation);  // unused type_index
  uint32_t size = DecodeUnsignedLeb128(&annotation);

  while (size != 0) {
    uint32_t element_name_index = DecodeUnsignedLeb128(&annotation);
    const char* element_name =
        dex_file.GetStringData(dex_file.GetStringId(dex::StringIndex(element_name_index)));
    if (strcmp(name, element_name) == 0) {
      return annotation;
    }
    SkipAnnotationValue(dex_file, &annotation);
    size--;
  }
  return nullptr;
}

static const AnnotationSetItem* FindAnnotationSetForMethod(const DexFile& dex_file,
                                                           const dex::ClassDef& class_def,
                                                           uint32_t method_index) {
  const AnnotationsDirectoryItem* annotations_dir = dex_file.GetAnnotationsDirectory(class_def);
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  const MethodAnnotationsItem* method_annotations = dex_file.GetMethodAnnotations(annotations_dir);
  if (method_annotations == nullptr) {
    return nullptr;
  }
  uint32_t method_count = annotations_dir->methods_size_;
  for (uint32_t i = 0; i < method_count; ++i) {
    if (method_annotations[i].method_idx_ == method_index) {
      return dex_file.GetMethodAnnotationSetItem(method_annotations[i]);
    }
  }
  return nullptr;
}

inline const AnnotationSetItem* FindAnnotationSetForMethod(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (method->IsProxyMethod()) {
    return nullptr;
  }
  return FindAnnotationSetForMethod(*method->GetDexFile(),
                                    method->GetClassDef(),
                                    method->GetDexMethodIndex());
}

const ParameterAnnotationsItem* FindAnnotationsItemForMethod(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile* dex_file = method->GetDexFile();
  const AnnotationsDirectoryItem* annotations_dir =
      dex_file->GetAnnotationsDirectory(method->GetClassDef());
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  const ParameterAnnotationsItem* parameter_annotations =
      dex_file->GetParameterAnnotations(annotations_dir);
  if (parameter_annotations == nullptr) {
    return nullptr;
  }
  uint32_t method_index = method->GetDexMethodIndex();
  uint32_t parameter_count = annotations_dir->parameters_size_;
  for (uint32_t i = 0; i < parameter_count; ++i) {
    if (parameter_annotations[i].method_idx_ == method_index) {
      return &parameter_annotations[i];
    }
  }
  return nullptr;
}

static const AnnotationSetItem* FindAnnotationSetForClass(const ClassData& klass)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = klass.GetDexFile();
  const dex::ClassDef* class_def = klass.GetClassDef();
  if (class_def == nullptr) {
    DCHECK(klass.GetRealClass()->IsProxyClass());
    return nullptr;
  }
  const AnnotationsDirectoryItem* annotations_dir = dex_file.GetAnnotationsDirectory(*class_def);
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  return dex_file.GetClassAnnotationSet(annotations_dir);
}

ObjPtr<mirror::Object> ProcessEncodedAnnotation(const ClassData& klass, const uint8_t** annotation)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  uint32_t type_index = DecodeUnsignedLeb128(annotation);
  uint32_t size = DecodeUnsignedLeb128(annotation);

  Thread* self = Thread::Current();
  StackHandleScope<4> hs(self);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> annotation_class(hs.NewHandle(
      class_linker->ResolveType(dex::TypeIndex(type_index),
                                hs.NewHandle(klass.GetDexCache()),
                                hs.NewHandle(klass.GetClassLoader()))));
  if (annotation_class == nullptr) {
    LOG(INFO) << "Unable to resolve " << klass.GetRealClass()->PrettyClass()
              << " annotation class " << type_index;
    DCHECK(Thread::Current()->IsExceptionPending());
    Thread::Current()->ClearException();
    return nullptr;
  }

  ObjPtr<mirror::Class> annotation_member_array_class =
      WellKnownClasses::ToClass(WellKnownClasses::libcore_reflect_AnnotationMember__array);
  if (annotation_member_array_class == nullptr) {
    return nullptr;
  }
  ObjPtr<mirror::ObjectArray<mirror::Object>> element_array = nullptr;
  if (size > 0) {
    element_array =
        mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_member_array_class, size);
    if (element_array == nullptr) {
      LOG(ERROR) << "Failed to allocate annotation member array (" << size << " elements)";
      return nullptr;
    }
  }

  Handle<mirror::ObjectArray<mirror::Object>> h_element_array(hs.NewHandle(element_array));
  for (uint32_t i = 0; i < size; ++i) {
    ObjPtr<mirror::Object> new_member = CreateAnnotationMember(klass, annotation_class, annotation);
    if (new_member == nullptr) {
      return nullptr;
    }
    h_element_array->SetWithoutChecks<false>(i, new_member);
  }

  ArtMethod* create_annotation_method =
      WellKnownClasses::libcore_reflect_AnnotationFactory_createAnnotation;
  ObjPtr<mirror::Object> result = create_annotation_method->InvokeStatic<'L', 'L', 'L'>(
      self, annotation_class.Get(), h_element_array.Get());
  if (self->IsExceptionPending()) {
    LOG(INFO) << "Exception in AnnotationFactory.createAnnotation";
    return nullptr;
  }

  return result;
}

template <bool kTransactionActive>
bool ProcessAnnotationValue(const ClassData& klass,
                            const uint8_t** annotation_ptr,
                            DexFile::AnnotationValue* annotation_value,
                            Handle<mirror::Class> array_class,
                            DexFile::AnnotationResultStyle result_style)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = klass.GetDexFile();
  Thread* self = Thread::Current();
  ObjPtr<mirror::Object> element_object = nullptr;
  bool set_object = false;
  Primitive::Type primitive_type = Primitive::kPrimVoid;
  const uint8_t* annotation = *annotation_ptr;
  uint8_t header_byte = *(annotation++);
  uint8_t value_type = header_byte & DexFile::kDexAnnotationValueTypeMask;
  uint8_t value_arg = header_byte >> DexFile::kDexAnnotationValueArgShift;
  int32_t width = value_arg + 1;
  annotation_value->type_ = value_type;

  switch (value_type) {
    case DexFile::kDexAnnotationByte:
      annotation_value->value_.SetB(
          static_cast<int8_t>(DexFile::ReadSignedInt(annotation, value_arg)));
      primitive_type = Primitive::kPrimByte;
      break;
    case DexFile::kDexAnnotationShort:
      annotation_value->value_.SetS(
          static_cast<int16_t>(DexFile::ReadSignedInt(annotation, value_arg)));
      primitive_type = Primitive::kPrimShort;
      break;
    case DexFile::kDexAnnotationChar:
      annotation_value->value_.SetC(
          static_cast<uint16_t>(DexFile::ReadUnsignedInt(annotation, value_arg, false)));
      primitive_type = Primitive::kPrimChar;
      break;
    case DexFile::kDexAnnotationInt:
      annotation_value->value_.SetI(DexFile::ReadSignedInt(annotation, value_arg));
      primitive_type = Primitive::kPrimInt;
      break;
    case DexFile::kDexAnnotationLong:
      annotation_value->value_.SetJ(DexFile::ReadSignedLong(annotation, value_arg));
      primitive_type = Primitive::kPrimLong;
      break;
    case DexFile::kDexAnnotationFloat:
      annotation_value->value_.SetI(DexFile::ReadUnsignedInt(annotation, value_arg, true));
      primitive_type = Primitive::kPrimFloat;
      break;
    case DexFile::kDexAnnotationDouble:
      annotation_value->value_.SetJ(DexFile::ReadUnsignedLong(annotation, value_arg, true));
      primitive_type = Primitive::kPrimDouble;
      break;
    case DexFile::kDexAnnotationBoolean:
      annotation_value->value_.SetZ(value_arg != 0);
      primitive_type = Primitive::kPrimBoolean;
      width = 0;
      break;
    case DexFile::kDexAnnotationString: {
      uint32_t index = DexFile::ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == DexFile::kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        StackHandleScope<1> hs(self);
        element_object = Runtime::Current()->GetClassLinker()->ResolveString(
            dex::StringIndex(index), hs.NewHandle(klass.GetDexCache()));
        set_object = true;
        if (element_object == nullptr) {
          return false;
        }
      }
      break;
    }
    case DexFile::kDexAnnotationType: {
      uint32_t index = DexFile::ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == DexFile::kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        dex::TypeIndex type_index(index);
        StackHandleScope<2> hs(self);
        element_object = Runtime::Current()->GetClassLinker()->ResolveType(
            type_index,
            hs.NewHandle(klass.GetDexCache()),
            hs.NewHandle(klass.GetClassLoader()));
        set_object = true;
        if (element_object == nullptr) {
          CHECK(self->IsExceptionPending());
          if (result_style == DexFile::kAllObjects) {
            const char* msg = dex_file.GetTypeDescriptor(type_index);
            self->ThrowNewWrappedException("Ljava/lang/TypeNotPresentException;", msg);
            element_object = self->GetException();
            self->ClearException();
          } else {
            return false;
          }
        }
      }
      break;
    }
    case DexFile::kDexAnnotationMethod: {
      uint32_t index = DexFile::ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == DexFile::kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
        StackHandleScope<2> hs(self);
        ArtMethod* method = class_linker->ResolveMethodId(
            index,
            hs.NewHandle(klass.GetDexCache()),
            hs.NewHandle(klass.GetClassLoader()));
        if (method == nullptr) {
          return false;
        }
        PointerSize pointer_size = class_linker->GetImagePointerSize();
        set_object = true;
        if (method->IsConstructor()) {
          element_object = (pointer_size == PointerSize::k64)
              ? mirror::Constructor::CreateFromArtMethod<PointerSize::k64>(self, method)
              : mirror::Constructor::CreateFromArtMethod<PointerSize::k32>(self, method);
        } else {
          element_object = (pointer_size == PointerSize::k64)
              ? mirror::Method::CreateFromArtMethod<PointerSize::k64>(self, method)
              : mirror::Method::CreateFromArtMethod<PointerSize::k32>(self, method);
        }
        if (element_object == nullptr) {
          return false;
        }
      }
      break;
    }
    case DexFile::kDexAnnotationField: {
      uint32_t index = DexFile::ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == DexFile::kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        StackHandleScope<2> hs(self);
        ArtField* field = Runtime::Current()->GetClassLinker()->ResolveFieldJLS(
            index,
            hs.NewHandle(klass.GetDexCache()),
            hs.NewHandle(klass.GetClassLoader()));
        if (field == nullptr) {
          return false;
        }
        set_object = true;
        element_object = mirror::Field::CreateFromArtField(self, field, true);
        if (element_object == nullptr) {
          return false;
        }
      }
      break;
    }
    case DexFile::kDexAnnotationEnum: {
      uint32_t index = DexFile::ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == DexFile::kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        StackHandleScope<3> hs(self);
        ArtField* enum_field = Runtime::Current()->GetClassLinker()->ResolveField(
            index,
            hs.NewHandle(klass.GetDexCache()),
            hs.NewHandle(klass.GetClassLoader()),
            true);
        if (enum_field == nullptr) {
          return false;
        } else {
          Handle<mirror::Class> field_class(hs.NewHandle(enum_field->GetDeclaringClass()));
          Runtime::Current()->GetClassLinker()->EnsureInitialized(self, field_class, true, true);
          element_object = enum_field->GetObject(field_class.Get());
          set_object = true;
        }
      }
      break;
    }
    case DexFile::kDexAnnotationArray:
      if (result_style == DexFile::kAllRaw || array_class == nullptr) {
        return false;
      } else {
        ScopedObjectAccessUnchecked soa(self);
        StackHandleScope<2> hs(self);
        uint32_t size = DecodeUnsignedLeb128(&annotation);
        Handle<mirror::Class> component_type(hs.NewHandle(array_class->GetComponentType()));
        Handle<mirror::Array> new_array(hs.NewHandle(mirror::Array::Alloc(
            self, array_class.Get(), size, array_class->GetComponentSizeShift(),
            Runtime::Current()->GetHeap()->GetCurrentAllocator())));
        if (new_array == nullptr) {
          LOG(ERROR) << "Annotation element array allocation failed with size " << size;
          return false;
        }
        DexFile::AnnotationValue new_annotation_value;
        for (uint32_t i = 0; i < size; ++i) {
          if (!ProcessAnnotationValue<kTransactionActive>(klass,
                                                          &annotation,
                                                          &new_annotation_value,
                                                          component_type,
                                                          DexFile::kPrimitivesOrObjects)) {
            return false;
          }
          if (!component_type->IsPrimitive()) {
            ObjPtr<mirror::Object> obj = new_annotation_value.value_.GetL();
            new_array->AsObjectArray<mirror::Object>()->
                SetWithoutChecks<kTransactionActive>(i, obj);
          } else {
            switch (new_annotation_value.type_) {
              case DexFile::kDexAnnotationByte:
                new_array->AsByteArray()->SetWithoutChecks<kTransactionActive>(
                    i, new_annotation_value.value_.GetB());
                break;
              case DexFile::kDexAnnotationShort:
                new_array->AsShortArray()->SetWithoutChecks<kTransactionActive>(
                    i, new_annotation_value.value_.GetS());
                break;
              case DexFile::kDexAnnotationChar:
                new_array->AsCharArray()->SetWithoutChecks<kTransactionActive>(
                    i, new_annotation_value.value_.GetC());
                break;
              case DexFile::kDexAnnotationInt:
                new_array->AsIntArray()->SetWithoutChecks<kTransactionActive>(
                    i, new_annotation_value.value_.GetI());
                break;
              case DexFile::kDexAnnotationLong:
                new_array->AsLongArray()->SetWithoutChecks<kTransactionActive>(
                    i, new_annotation_value.value_.GetJ());
                break;
              case DexFile::kDexAnnotationFloat:
                new_array->AsFloatArray()->SetWithoutChecks<kTransactionActive>(
                    i, new_annotation_value.value_.GetF());
                break;
              case DexFile::kDexAnnotationDouble:
                new_array->AsDoubleArray()->SetWithoutChecks<kTransactionActive>(
                    i, new_annotation_value.value_.GetD());
                break;
              case DexFile::kDexAnnotationBoolean:
                new_array->AsBooleanArray()->SetWithoutChecks<kTransactionActive>(
                    i, new_annotation_value.value_.GetZ());
                break;
              default:
                LOG(FATAL) << "Found invalid annotation value type while building annotation array";
                return false;
            }
          }
        }
        element_object = new_array.Get();
        set_object = true;
        width = 0;
      }
      break;
    case DexFile::kDexAnnotationAnnotation:
      if (result_style == DexFile::kAllRaw) {
        return false;
      }
      element_object = ProcessEncodedAnnotation(klass, &annotation);
      if (element_object == nullptr) {
        return false;
      }
      set_object = true;
      width = 0;
      break;
    case DexFile::kDexAnnotationNull:
      if (result_style == DexFile::kAllRaw) {
        annotation_value->value_.SetI(0);
      } else {
        CHECK(element_object == nullptr);
        set_object = true;
      }
      width = 0;
      break;
    default:
      LOG(ERROR) << StringPrintf("Bad annotation element value type 0x%02x", value_type);
      return false;
  }

  annotation += width;
  *annotation_ptr = annotation;

  if (result_style == DexFile::kAllObjects && primitive_type != Primitive::kPrimVoid) {
    element_object = BoxPrimitive(primitive_type, annotation_value->value_);
    set_object = true;
  }

  if (set_object) {
    annotation_value->value_.SetL(element_object);
  }

  return true;
}

ObjPtr<mirror::Object> CreateAnnotationMember(const ClassData& klass,
                                              Handle<mirror::Class> annotation_class,
                                              const uint8_t** annotation) {
  const DexFile& dex_file = klass.GetDexFile();
  Thread* self = Thread::Current();
  ScopedObjectAccessUnchecked soa(self);
  StackHandleScope<5> hs(self);
  uint32_t element_name_index = DecodeUnsignedLeb128(annotation);
  const char* name = dex_file.GetStringData(dex::StringIndex(element_name_index));

  PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  ArtMethod* annotation_method =
      annotation_class->FindDeclaredVirtualMethodByName(name, pointer_size);
  if (annotation_method == nullptr) {
    return nullptr;
  }

  Handle<mirror::String> string_name =
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, name));
  if (UNLIKELY(string_name == nullptr)) {
    LOG(ERROR) << "Failed to allocate name for annotation member";
    return nullptr;
  }

  Handle<mirror::Class> method_return = hs.NewHandle(annotation_method->ResolveReturnType());
  if (UNLIKELY(method_return == nullptr)) {
    LOG(ERROR) << "Failed to resolve method return type for annotation member";
    return nullptr;
  }

  DexFile::AnnotationValue annotation_value;
  if (!ProcessAnnotationValue<false>(klass,
                                     annotation,
                                     &annotation_value,
                                     method_return,
                                     DexFile::kAllObjects)) {
    // TODO: Logging the error breaks run-test 005-annotations.
    // LOG(ERROR) << "Failed to process annotation value for annotation member";
    return nullptr;
  }
  Handle<mirror::Object> value_object = hs.NewHandle(annotation_value.value_.GetL());

  Handle<mirror::Method> method_object = hs.NewHandle((pointer_size == PointerSize::k64)
      ? mirror::Method::CreateFromArtMethod<PointerSize::k64>(self, annotation_method)
      : mirror::Method::CreateFromArtMethod<PointerSize::k32>(self, annotation_method));
  if (UNLIKELY(method_object == nullptr)) {
    LOG(ERROR) << "Failed to create method object for annotation member";
    return nullptr;
  }

  Handle<mirror::Object> new_member =
      WellKnownClasses::libcore_reflect_AnnotationMember_init->NewObject<'L', 'L', 'L', 'L'>(
          hs, self, string_name, value_object, method_return, method_object);
  if (new_member == nullptr) {
    DCHECK(self->IsExceptionPending());
    LOG(ERROR) << "Failed to create annotation member";
    return nullptr;
  }

  return new_member.Get();
}

const AnnotationItem* GetAnnotationItemFromAnnotationSet(const ClassData& klass,
                                                         const AnnotationSetItem* annotation_set,
                                                         uint32_t visibility,
                                                         Handle<mirror::Class> annotation_class)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = klass.GetDexFile();
  for (uint32_t i = 0; i < annotation_set->size_; ++i) {
    const AnnotationItem* annotation_item = dex_file.GetAnnotationItem(annotation_set, i);
    if (!IsVisibilityCompatible(annotation_item->visibility_, visibility)) {
      continue;
    }
    const uint8_t* annotation = annotation_item->annotation_;
    uint32_t type_index = DecodeUnsignedLeb128(&annotation);
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Thread* self = Thread::Current();
    StackHandleScope<2> hs(self);
    ObjPtr<mirror::Class> resolved_class = class_linker->ResolveType(
        dex::TypeIndex(type_index),
        hs.NewHandle(klass.GetDexCache()),
        hs.NewHandle(klass.GetClassLoader()));
    if (resolved_class == nullptr) {
      std::string temp;
      LOG(WARNING) << StringPrintf("Unable to resolve %s annotation class %d",
                                   klass.GetRealClass()->GetDescriptor(&temp), type_index);
      CHECK(self->IsExceptionPending());
      self->ClearException();
      continue;
    }
    if (resolved_class == annotation_class.Get()) {
      return annotation_item;
    }
  }

  return nullptr;
}

ObjPtr<mirror::Object> GetAnnotationObjectFromAnnotationSet(const ClassData& klass,
                                                            const AnnotationSetItem* annotation_set,
                                                            uint32_t visibility,
                                                            Handle<mirror::Class> annotation_class)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const AnnotationItem* annotation_item = GetAnnotationItemFromAnnotationSet(
      klass, annotation_set, visibility, annotation_class);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  const uint8_t* annotation = annotation_item->annotation_;
  return ProcessEncodedAnnotation(klass, &annotation);
}

ObjPtr<mirror::Object> GetAnnotationValue(const ClassData& klass,
                                          const AnnotationItem* annotation_item,
                                          const char* annotation_name,
                                          Handle<mirror::Class> array_class,
                                          uint32_t expected_type)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = klass.GetDexFile();
  const uint8_t* annotation =
      SearchEncodedAnnotation(dex_file, annotation_item->annotation_, annotation_name);
  if (annotation == nullptr) {
    return nullptr;
  }
  DexFile::AnnotationValue annotation_value;
  bool result = Runtime::Current()->IsActiveTransaction()
      ? ProcessAnnotationValue<true>(klass,
                                     &annotation,
                                     &annotation_value,
                                     array_class,
                                     DexFile::kAllObjects)
      : ProcessAnnotationValue<false>(klass,
                                      &annotation,
                                      &annotation_value,
                                      array_class,
                                      DexFile::kAllObjects);
  if (!result) {
    return nullptr;
  }
  if (annotation_value.type_ != expected_type) {
    return nullptr;
  }
  return annotation_value.value_.GetL();
}

template<typename T>
static inline ObjPtr<mirror::ObjectArray<T>> GetAnnotationArrayValue(
                                     Handle<mirror::Class> klass,
                                     const char* annotation_name,
                                     const char* value_name)
            REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(data.GetDexFile(), annotation_set, annotation_name,
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> class_array_class =
      hs.NewHandle(GetClassRoot<mirror::ObjectArray<T>>());
  DCHECK(class_array_class != nullptr);
  ObjPtr<mirror::Object> obj = GetAnnotationValue(data,
                                                  annotation_item,
                                                  value_name,
                                                  class_array_class,
                                                  DexFile::kDexAnnotationArray);
  if (obj == nullptr) {
    return nullptr;
  }
  return obj->AsObjectArray<T>();
}

static ObjPtr<mirror::ObjectArray<mirror::String>> GetSignatureValue(
    const ClassData& klass,
    const AnnotationSetItem* annotation_set)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = klass.GetDexFile();
  StackHandleScope<1> hs(Thread::Current());
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(dex_file, annotation_set, "Ldalvik/annotation/Signature;",
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  Handle<mirror::Class> string_array_class =
      hs.NewHandle(GetClassRoot<mirror::ObjectArray<mirror::String>>());
  DCHECK(string_array_class != nullptr);
  ObjPtr<mirror::Object> obj =
      GetAnnotationValue(klass, annotation_item, "value", string_array_class,
                         DexFile::kDexAnnotationArray);
  if (obj == nullptr) {
    return nullptr;
  }
  return obj->AsObjectArray<mirror::String>();
}

ObjPtr<mirror::ObjectArray<mirror::Class>> GetThrowsValue(const ClassData& klass,
                                                          const AnnotationSetItem* annotation_set)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = klass.GetDexFile();
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(dex_file, annotation_set, "Ldalvik/annotation/Throws;",
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> class_array_class =
      hs.NewHandle(GetClassRoot<mirror::ObjectArray<mirror::Class>>());
  DCHECK(class_array_class != nullptr);
  ObjPtr<mirror::Object> obj =
      GetAnnotationValue(klass, annotation_item, "value", class_array_class,
                         DexFile::kDexAnnotationArray);
  if (obj == nullptr) {
    return nullptr;
  }
  return obj->AsObjectArray<mirror::Class>();
}

ObjPtr<mirror::ObjectArray<mirror::Object>> ProcessAnnotationSet(
    const ClassData& klass,
    const AnnotationSetItem* annotation_set,
    uint32_t visibility)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = klass.GetDexFile();
  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> annotation_array_class(hs.NewHandle(
      WellKnownClasses::ToClass(WellKnownClasses::java_lang_annotation_Annotation__array)));
  if (annotation_set == nullptr) {
    return mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_array_class.Get(), 0);
  }

  uint32_t size = annotation_set->size_;
  Handle<mirror::ObjectArray<mirror::Object>> result(hs.NewHandle(
      mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_array_class.Get(), size)));
  if (result == nullptr) {
    return nullptr;
  }

  uint32_t dest_index = 0;
  for (uint32_t i = 0; i < size; ++i) {
    const AnnotationItem* annotation_item = dex_file.GetAnnotationItem(annotation_set, i);
    // Note that we do not use IsVisibilityCompatible here because older code
    // was correct for this case.
    if (annotation_item->visibility_ != visibility) {
      continue;
    }
    const uint8_t* annotation = annotation_item->annotation_;
    ObjPtr<mirror::Object> annotation_obj = ProcessEncodedAnnotation(klass, &annotation);
    if (annotation_obj != nullptr) {
      result->SetWithoutChecks<false>(dest_index, annotation_obj);
      ++dest_index;
    } else if (self->IsExceptionPending()) {
      return nullptr;
    }
  }

  if (dest_index == size) {
    return result.Get();
  }

  ObjPtr<mirror::ObjectArray<mirror::Object>> trimmed_result =
      mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_array_class.Get(), dest_index);
  if (trimmed_result == nullptr) {
    return nullptr;
  }

  for (uint32_t i = 0; i < dest_index; ++i) {
    ObjPtr<mirror::Object> obj = result->GetWithoutChecks(i);
    trimmed_result->SetWithoutChecks<false>(i, obj);
  }

  return trimmed_result;
}

ObjPtr<mirror::ObjectArray<mirror::Object>> ProcessAnnotationSetRefList(
    const ClassData& klass,
    const AnnotationSetRefList* set_ref_list,
    uint32_t size)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile& dex_file = klass.GetDexFile();
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  ObjPtr<mirror::Class> annotation_array_class =
      WellKnownClasses::ToClass(WellKnownClasses::java_lang_annotation_Annotation__array);
  ObjPtr<mirror::Class> annotation_array_array_class =
      Runtime::Current()->GetClassLinker()->FindArrayClass(self, annotation_array_class);
  if (annotation_array_array_class == nullptr) {
    return nullptr;
  }
  Handle<mirror::ObjectArray<mirror::Object>> annotation_array_array(hs.NewHandle(
      mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_array_array_class, size)));
  if (annotation_array_array == nullptr) {
    LOG(ERROR) << "Annotation set ref array allocation failed";
    return nullptr;
  }
  for (uint32_t index = 0; index < size; ++index) {
    const AnnotationSetRefItem* set_ref_item = &set_ref_list->list_[index];
    const AnnotationSetItem* set_item = dex_file.GetSetRefItemItem(set_ref_item);
    ObjPtr<mirror::Object> annotation_set = ProcessAnnotationSet(klass,
                                                                 set_item,
                                                                 DexFile::kDexVisibilityRuntime);
    if (annotation_set == nullptr) {
      return nullptr;
    }
    annotation_array_array->SetWithoutChecks<false>(index, annotation_set);
  }
  return annotation_array_array.Get();
}
}  // namespace

namespace annotations {

ObjPtr<mirror::Object> GetAnnotationForField(ArtField* field,
                                             Handle<mirror::Class> annotation_class) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForField(field);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  const ClassData field_class(hs, field);
  return GetAnnotationObjectFromAnnotationSet(field_class,
                                              annotation_set,
                                              DexFile::kDexVisibilityRuntime,
                                              annotation_class);
}

ObjPtr<mirror::ObjectArray<mirror::Object>> GetAnnotationsForField(ArtField* field) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForField(field);
  StackHandleScope<1> hs(Thread::Current());
  const ClassData field_class(hs, field);
  return ProcessAnnotationSet(field_class, annotation_set, DexFile::kDexVisibilityRuntime);
}

ObjPtr<mirror::ObjectArray<mirror::String>> GetSignatureAnnotationForField(ArtField* field) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForField(field);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  const ClassData field_class(hs, field);
  return GetSignatureValue(field_class, annotation_set);
}

bool IsFieldAnnotationPresent(ArtField* field, Handle<mirror::Class> annotation_class) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForField(field);
  if (annotation_set == nullptr) {
    return false;
  }
  StackHandleScope<1> hs(Thread::Current());
  const ClassData field_class(hs, field);
  const AnnotationItem* annotation_item = GetAnnotationItemFromAnnotationSet(
      field_class, annotation_set, DexFile::kDexVisibilityRuntime, annotation_class);
  return annotation_item != nullptr;
}

ObjPtr<mirror::Object> GetAnnotationDefaultValue(ArtMethod* method) {
  const ClassData klass(method);
  const DexFile* dex_file = &klass.GetDexFile();
  const AnnotationsDirectoryItem* annotations_dir =
      dex_file->GetAnnotationsDirectory(*klass.GetClassDef());
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  const AnnotationSetItem* annotation_set =
      dex_file->GetClassAnnotationSet(annotations_dir);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(*dex_file, annotation_set,
      "Ldalvik/annotation/AnnotationDefault;", DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  const uint8_t* annotation =
      SearchEncodedAnnotation(*dex_file, annotation_item->annotation_, "value");
  if (annotation == nullptr) {
    return nullptr;
  }
  uint8_t header_byte = *(annotation++);
  if ((header_byte & DexFile::kDexAnnotationValueTypeMask) != DexFile::kDexAnnotationAnnotation) {
    return nullptr;
  }
  annotation = SearchEncodedAnnotation(*dex_file, annotation, method->GetName());
  if (annotation == nullptr) {
    return nullptr;
  }
  DexFile::AnnotationValue annotation_value;
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> return_type(hs.NewHandle(method->ResolveReturnType()));
  if (!ProcessAnnotationValue<false>(klass,
                                     &annotation,
                                     &annotation_value,
                                     return_type,
                                     DexFile::kAllObjects)) {
    return nullptr;
  }
  return annotation_value.value_.GetL();
}

ObjPtr<mirror::Object> GetAnnotationForMethod(ArtMethod* method,
                                              Handle<mirror::Class> annotation_class) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  return GetAnnotationObjectFromAnnotationSet(ClassData(method), annotation_set,
                                              DexFile::kDexVisibilityRuntime, annotation_class);
}

ObjPtr<mirror::ObjectArray<mirror::Object>> GetAnnotationsForMethod(ArtMethod* method) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  return ProcessAnnotationSet(ClassData(method),
                              annotation_set,
                              DexFile::kDexVisibilityRuntime);
}

ObjPtr<mirror::ObjectArray<mirror::Class>> GetExceptionTypesForMethod(ArtMethod* method) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  return GetThrowsValue(ClassData(method), annotation_set);
}

ObjPtr<mirror::ObjectArray<mirror::Object>> GetParameterAnnotations(ArtMethod* method) {
  const DexFile* dex_file = method->GetDexFile();
  const ParameterAnnotationsItem* parameter_annotations =
      FindAnnotationsItemForMethod(method);
  if (parameter_annotations == nullptr) {
    return nullptr;
  }
  const AnnotationSetRefList* set_ref_list =
      dex_file->GetParameterAnnotationSetRefList(parameter_annotations);
  if (set_ref_list == nullptr) {
    return nullptr;
  }
  uint32_t size = set_ref_list->size_;
  return ProcessAnnotationSetRefList(ClassData(method), set_ref_list, size);
}

uint32_t GetNumberOfAnnotatedMethodParameters(ArtMethod* method) {
  const DexFile* dex_file = method->GetDexFile();
  const ParameterAnnotationsItem* parameter_annotations =
      FindAnnotationsItemForMethod(method);
  if (parameter_annotations == nullptr) {
    return 0u;
  }
  const AnnotationSetRefList* set_ref_list =
      dex_file->GetParameterAnnotationSetRefList(parameter_annotations);
  if (set_ref_list == nullptr) {
    return 0u;
  }
  return set_ref_list->size_;
}

ObjPtr<mirror::Object> GetAnnotationForMethodParameter(ArtMethod* method,
                                                       uint32_t parameter_idx,
                                                       Handle<mirror::Class> annotation_class) {
  const DexFile* dex_file = method->GetDexFile();
  const ParameterAnnotationsItem* parameter_annotations = FindAnnotationsItemForMethod(method);
  if (parameter_annotations == nullptr) {
    return nullptr;
  }
  const AnnotationSetRefList* set_ref_list =
      dex_file->GetParameterAnnotationSetRefList(parameter_annotations);
  if (set_ref_list == nullptr) {
    return nullptr;
  }
  if (parameter_idx >= set_ref_list->size_) {
    return nullptr;
  }
  const AnnotationSetRefItem* annotation_set_ref = &set_ref_list->list_[parameter_idx];
  const AnnotationSetItem* annotation_set =
     dex_file->GetSetRefItemItem(annotation_set_ref);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  return GetAnnotationObjectFromAnnotationSet(ClassData(method),
                                              annotation_set,
                                              DexFile::kDexVisibilityRuntime,
                                              annotation_class);
}

bool GetParametersMetadataForMethod(
    ArtMethod* method,
    /*out*/ MutableHandle<mirror::ObjectArray<mirror::String>>* names,
    /*out*/ MutableHandle<mirror::IntArray>* access_flags) {
  const AnnotationSetItem* annotation_set =
      FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return false;
  }

  const DexFile* dex_file = method->GetDexFile();
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(*dex_file,
                          annotation_set,
                          "Ldalvik/annotation/MethodParameters;",
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return false;
  }

  StackHandleScope<4> hs(Thread::Current());

  // Extract the parameters' names String[].
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> string_array_class =
      hs.NewHandle(GetClassRoot<mirror::ObjectArray<mirror::String>>(class_linker));
  DCHECK(string_array_class != nullptr);

  ClassData data(method);
  Handle<mirror::Object> names_obj =
      hs.NewHandle(GetAnnotationValue(data,
                                      annotation_item,
                                      "names",
                                      string_array_class,
                                      DexFile::kDexAnnotationArray));
  if (names_obj == nullptr) {
    return false;
  }

  // Extract the parameters' access flags int[].
  Handle<mirror::Class> int_array_class(hs.NewHandle(GetClassRoot<mirror::IntArray>(class_linker)));
  DCHECK(int_array_class != nullptr);
  Handle<mirror::Object> access_flags_obj =
      hs.NewHandle(GetAnnotationValue(data,
                                      annotation_item,
                                      "accessFlags",
                                      int_array_class,
                                      DexFile::kDexAnnotationArray));
  if (access_flags_obj == nullptr) {
    return false;
  }

  names->Assign(names_obj->AsObjectArray<mirror::String>());
  access_flags->Assign(access_flags_obj->AsIntArray());
  return true;
}

ObjPtr<mirror::ObjectArray<mirror::String>> GetSignatureAnnotationForMethod(ArtMethod* method) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  return GetSignatureValue(ClassData(method), annotation_set);
}

bool IsMethodAnnotationPresent(ArtMethod* method,
                               Handle<mirror::Class> annotation_class,
                               uint32_t visibility /* = DexFile::kDexVisibilityRuntime */) {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = GetAnnotationItemFromAnnotationSet(
      ClassData(method), annotation_set, visibility, annotation_class);
  return annotation_item != nullptr;
}

static void DCheckNativeAnnotation(const char* descriptor, jclass cls) {
  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    ObjPtr<mirror::Class> klass = soa.Decode<mirror::Class>(cls);
    ClassLinker* linker = Runtime::Current()->GetClassLinker();
    // WellKnownClasses may not be initialized yet, so `klass` may be null.
    if (klass != nullptr) {
      // Lookup using the boot class path loader should yield the annotation class.
      CHECK_EQ(klass, linker->LookupClass(soa.Self(), descriptor, /* class_loader= */ nullptr));
    }
  }
}

// Check whether a method from the `dex_file` with the given `annotation_set`
// is annotated with `annotation_descriptor` with build visibility.
static bool IsMethodBuildAnnotationPresent(const DexFile& dex_file,
                                           const AnnotationSetItem& annotation_set,
                                           const char* annotation_descriptor,
                                           jclass annotation_class) {
  for (uint32_t i = 0; i < annotation_set.size_; ++i) {
    const AnnotationItem* annotation_item = dex_file.GetAnnotationItem(&annotation_set, i);
    if (!IsVisibilityCompatible(annotation_item->visibility_, DexFile::kDexVisibilityBuild)) {
      continue;
    }
    const uint8_t* annotation = annotation_item->annotation_;
    uint32_t type_index = DecodeUnsignedLeb128(&annotation);
    const char* descriptor = dex_file.GetTypeDescriptor(dex::TypeIndex(type_index));
    if (strcmp(descriptor, annotation_descriptor) == 0) {
      DCheckNativeAnnotation(descriptor, annotation_class);
      return true;
    }
  }
  return false;
}

static uint32_t GetNativeMethodAnnotationAccessFlags(const DexFile& dex_file,
                                                     const dex::AnnotationSetItem& annotation_set) {
  uint32_t access_flags = 0u;
  if (IsMethodBuildAnnotationPresent(
          dex_file,
          annotation_set,
          "Ldalvik/annotation/optimization/FastNative;",
          WellKnownClasses::dalvik_annotation_optimization_FastNative)) {
    access_flags |= kAccFastNative;
  }
  if (IsMethodBuildAnnotationPresent(
          dex_file,
          annotation_set,
          "Ldalvik/annotation/optimization/CriticalNative;",
          WellKnownClasses::dalvik_annotation_optimization_CriticalNative)) {
    access_flags |= kAccCriticalNative;
  }
  CHECK_NE(access_flags, kAccFastNative | kAccCriticalNative);
  return access_flags;
}

uint32_t GetNativeMethodAnnotationAccessFlags(const DexFile& dex_file,
                                              const dex::ClassDef& class_def,
                                              uint32_t method_index) {
  const dex::AnnotationSetItem* annotation_set =
      FindAnnotationSetForMethod(dex_file, class_def, method_index);
  if (annotation_set == nullptr) {
    return 0u;
  }
  return GetNativeMethodAnnotationAccessFlags(dex_file, *annotation_set);
}

uint32_t GetNativeMethodAnnotationAccessFlags(const DexFile& dex_file,
                                              const dex::MethodAnnotationsItem& method_annotations) {
  return GetNativeMethodAnnotationAccessFlags(
      dex_file, *dex_file.GetMethodAnnotationSetItem(method_annotations));
}

static bool MethodIsNeverCompile(const DexFile& dex_file,
                                 const dex::AnnotationSetItem& annotation_set) {
  return IsMethodBuildAnnotationPresent(
      dex_file,
      annotation_set,
      "Ldalvik/annotation/optimization/NeverCompile;",
      WellKnownClasses::dalvik_annotation_optimization_NeverCompile);
}

bool MethodIsNeverCompile(const DexFile& dex_file,
                          const dex::ClassDef& class_def,
                          uint32_t method_index) {
  const dex::AnnotationSetItem* annotation_set =
      FindAnnotationSetForMethod(dex_file, class_def, method_index);
  if (annotation_set == nullptr) {
    return false;
  }
  return MethodIsNeverCompile(dex_file, *annotation_set);
}

bool MethodIsNeverCompile(const DexFile& dex_file,
                          const dex::MethodAnnotationsItem& method_annotations) {
  return MethodIsNeverCompile(dex_file, *dex_file.GetMethodAnnotationSetItem(method_annotations));
}

bool MethodIsNeverInline(const DexFile& dex_file,
                         const dex::ClassDef& class_def,
                         uint32_t method_index) {
  const dex::AnnotationSetItem* annotation_set =
      FindAnnotationSetForMethod(dex_file, class_def, method_index);
  if (annotation_set == nullptr) {
    return false;
  }
  return IsMethodBuildAnnotationPresent(
      dex_file,
      *annotation_set,
      "Ldalvik/annotation/optimization/NeverInline;",
      WellKnownClasses::dalvik_annotation_optimization_NeverInline);
}

bool FieldIsReachabilitySensitive(const DexFile& dex_file,
                                  const dex::ClassDef& class_def,
                                  uint32_t field_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const AnnotationSetItem* annotation_set =
      FindAnnotationSetForField(dex_file, class_def, field_index);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(dex_file, annotation_set,
      "Ldalvik/annotation/optimization/ReachabilitySensitive;", DexFile::kDexVisibilityRuntime);
  // TODO: We're missing the equivalent of DCheckNativeAnnotation (not a DCHECK). Does it matter?
  return annotation_item != nullptr;
}

bool MethodIsReachabilitySensitive(const DexFile& dex_file,
                                   const dex::ClassDef& class_def,
                                   uint32_t method_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const AnnotationSetItem* annotation_set =
      FindAnnotationSetForMethod(dex_file, class_def, method_index);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(dex_file, annotation_set,
      "Ldalvik/annotation/optimization/ReachabilitySensitive;", DexFile::kDexVisibilityRuntime);
  return annotation_item != nullptr;
}

static bool MethodIsReachabilitySensitive(const DexFile& dex_file,
                                               uint32_t method_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(method_index < dex_file.NumMethodIds());
  const dex::MethodId& method_id = dex_file.GetMethodId(method_index);
  dex::TypeIndex class_index = method_id.class_idx_;
  const dex::ClassDef * class_def = dex_file.FindClassDef(class_index);
  return class_def != nullptr
         && MethodIsReachabilitySensitive(dex_file, *class_def, method_index);
}

bool MethodContainsRSensitiveAccess(const DexFile& dex_file,
                                    const dex::ClassDef& class_def,
                                    uint32_t method_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // TODO: This is too slow to run very regularly. Currently this is only invoked in the
  // presence of @DeadReferenceSafe, which will be rare. In the long run, we need to quickly
  // check once whether a class has any @ReachabilitySensitive annotations. If not, we can
  // immediately return false here for any method in that class.
  uint32_t code_item_offset = dex_file.FindCodeItemOffset(class_def, method_index);
  const dex::CodeItem* code_item = dex_file.GetCodeItem(code_item_offset);
  CodeItemInstructionAccessor accessor(dex_file, code_item);
  if (!accessor.HasCodeItem()) {
    return false;
  }
  for (DexInstructionIterator iter = accessor.begin(); iter != accessor.end(); ++iter) {
    switch (iter->Opcode()) {
      case Instruction::IGET:
      case Instruction::IGET_WIDE:
      case Instruction::IGET_OBJECT:
      case Instruction::IGET_BOOLEAN:
      case Instruction::IGET_BYTE:
      case Instruction::IGET_CHAR:
      case Instruction::IGET_SHORT:
      case Instruction::IPUT:
      case Instruction::IPUT_WIDE:
      case Instruction::IPUT_OBJECT:
      case Instruction::IPUT_BOOLEAN:
      case Instruction::IPUT_BYTE:
      case Instruction::IPUT_CHAR:
      case Instruction::IPUT_SHORT:
        {
          uint32_t field_index = iter->VRegC_22c();
          DCHECK(field_index < dex_file.NumFieldIds());
          // We only guarantee to pay attention to the annotation if it's in the same class,
          // or a containing class, but it's OK to do so in other cases.
          const dex::FieldId& field_id = dex_file.GetFieldId(field_index);
          dex::TypeIndex class_index = field_id.class_idx_;
          const dex::ClassDef * field_class_def = dex_file.FindClassDef(class_index);
          // We do not handle the case in which the field is declared in a superclass, and
          // don't claim to do so. The annotated field should normally be private.
          if (field_class_def != nullptr
              && FieldIsReachabilitySensitive(dex_file, *field_class_def, field_index)) {
            return true;
          }
        }
        break;
      case Instruction::INVOKE_SUPER:
        // Cannot call method in same class. TODO: Try an explicit superclass lookup for
        // better "best effort"?
        break;
      case Instruction::INVOKE_INTERFACE:
        // We handle an interface call just like a virtual call. We will find annotations
        // on interface methods/fields visible to us, but not of the annotation is in a
        // super-interface. Again, we could just ignore it.
      case Instruction::INVOKE_VIRTUAL:
      case Instruction::INVOKE_DIRECT:
        {
          uint32_t called_method_index = iter->VRegB_35c();
          if (MethodIsReachabilitySensitive(dex_file, called_method_index)) {
            return true;
          }
        }
        break;
      case Instruction::INVOKE_INTERFACE_RANGE:
      case Instruction::INVOKE_VIRTUAL_RANGE:
      case Instruction::INVOKE_DIRECT_RANGE:
        {
          uint32_t called_method_index = iter->VRegB_3rc();
          if (MethodIsReachabilitySensitive(dex_file, called_method_index)) {
            return true;
          }
        }
        break;
        // We explicitly do not handle indirect ReachabilitySensitive accesses through VarHandles,
        // etc. Thus we ignore INVOKE_CUSTOM / INVOKE_CUSTOM_RANGE / INVOKE_POLYMORPHIC /
        // INVOKE_POLYMORPHIC_RANGE.
      default:
        // There is no way to add an annotation to array elements, and so far we've encountered no
        // need for that, so we ignore AGET and APUT.
        // It's impractical or impossible to garbage collect a class while one of its methods is
        // on the call stack. We allow ReachabilitySensitive annotations on static methods and
        // fields, but they can be safely ignored.
        break;
    }
  }
  return false;
}

bool HasDeadReferenceSafeAnnotation(const DexFile& dex_file,
                                    const dex::ClassDef& class_def)
  // TODO: This should check outer classes as well.
  // It's conservatively correct not to do so.
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const AnnotationsDirectoryItem* annotations_dir =
      dex_file.GetAnnotationsDirectory(class_def);
  if (annotations_dir == nullptr) {
    return false;
  }
  const AnnotationSetItem* annotation_set = dex_file.GetClassAnnotationSet(annotations_dir);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(dex_file, annotation_set,
      "Ldalvik/annotation/optimization/DeadReferenceSafe;", DexFile::kDexVisibilityRuntime);
  return annotation_item != nullptr;
}

ObjPtr<mirror::Object> GetAnnotationForClass(Handle<mirror::Class> klass,
                                             Handle<mirror::Class> annotation_class) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  return GetAnnotationObjectFromAnnotationSet(data,
                                              annotation_set,
                                              DexFile::kDexVisibilityRuntime,
                                              annotation_class);
}

ObjPtr<mirror::ObjectArray<mirror::Object>> GetAnnotationsForClass(Handle<mirror::Class> klass) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  return ProcessAnnotationSet(data, annotation_set, DexFile::kDexVisibilityRuntime);
}

ObjPtr<mirror::ObjectArray<mirror::Class>> GetDeclaredClasses(Handle<mirror::Class> klass) {
  return GetAnnotationArrayValue<mirror::Class>(klass,
                                                "Ldalvik/annotation/MemberClasses;",
                                                "value");
}

ObjPtr<mirror::Class> GetDeclaringClass(Handle<mirror::Class> klass) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(data.GetDexFile(), annotation_set, "Ldalvik/annotation/EnclosingClass;",
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  ObjPtr<mirror::Object> obj = GetAnnotationValue(data,
                                                  annotation_item,
                                                  "value",
                                                  ScopedNullHandle<mirror::Class>(),
                                                  DexFile::kDexAnnotationType);
  if (obj == nullptr) {
    return nullptr;
  }
  if (!obj->IsClass()) {
    // TypeNotPresentException, throw the NoClassDefFoundError.
    Thread::Current()->SetException(obj->AsThrowable()->GetCause());
    return nullptr;
  }
  return obj->AsClass();
}

ObjPtr<mirror::Class> GetEnclosingClass(Handle<mirror::Class> klass) {
  ObjPtr<mirror::Class> declaring_class = GetDeclaringClass(klass);
  if (declaring_class != nullptr || Thread::Current()->IsExceptionPending()) {
    return declaring_class;
  }
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(data.GetDexFile(),
                          annotation_set,
                          "Ldalvik/annotation/EnclosingMethod;",
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  const uint8_t* annotation =
      SearchEncodedAnnotation(data.GetDexFile(), annotation_item->annotation_, "value");
  if (annotation == nullptr) {
    return nullptr;
  }
  DexFile::AnnotationValue annotation_value;
  if (!ProcessAnnotationValue<false>(data,
                                     &annotation,
                                     &annotation_value,
                                     ScopedNullHandle<mirror::Class>(),
                                     DexFile::kAllRaw)) {
    return nullptr;
  }
  if (annotation_value.type_ != DexFile::kDexAnnotationMethod) {
    return nullptr;
  }
  StackHandleScope<2> hs(Thread::Current());
  ArtMethod* method = Runtime::Current()->GetClassLinker()->ResolveMethodId(
      annotation_value.value_.GetI(),
      hs.NewHandle(data.GetDexCache()),
      hs.NewHandle(data.GetClassLoader()));
  if (method == nullptr) {
    return nullptr;
  }
  return method->GetDeclaringClass();
}

ObjPtr<mirror::Object> GetEnclosingMethod(Handle<mirror::Class> klass) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(data.GetDexFile(),
                          annotation_set,
                          "Ldalvik/annotation/EnclosingMethod;",
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  return GetAnnotationValue(data, annotation_item, "value", ScopedNullHandle<mirror::Class>(),
      DexFile::kDexAnnotationMethod);
}

bool GetInnerClass(Handle<mirror::Class> klass, /*out*/ ObjPtr<mirror::String>* name) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(
      data.GetDexFile(),
      annotation_set,
      "Ldalvik/annotation/InnerClass;",
      DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return false;
  }
  const uint8_t* annotation =
      SearchEncodedAnnotation(data.GetDexFile(), annotation_item->annotation_, "name");
  if (annotation == nullptr) {
    return false;
  }
  DexFile::AnnotationValue annotation_value;
  if (!ProcessAnnotationValue<false>(data,
                                     &annotation,
                                     &annotation_value,
                                     ScopedNullHandle<mirror::Class>(),
                                     DexFile::kAllObjects)) {
    return false;
  }
  if (annotation_value.type_ != DexFile::kDexAnnotationNull &&
      annotation_value.type_ != DexFile::kDexAnnotationString) {
    return false;
  }
  *name = down_cast<mirror::String*>(annotation_value.value_.GetL());
  return true;
}

bool GetInnerClassFlags(Handle<mirror::Class> klass, uint32_t* flags) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(data.GetDexFile(), annotation_set, "Ldalvik/annotation/InnerClass;",
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return false;
  }
  const uint8_t* annotation =
      SearchEncodedAnnotation(data.GetDexFile(), annotation_item->annotation_, "accessFlags");
  if (annotation == nullptr) {
    return false;
  }
  DexFile::AnnotationValue annotation_value;
  if (!ProcessAnnotationValue<false>(data,
                                     &annotation,
                                     &annotation_value,
                                     ScopedNullHandle<mirror::Class>(),
                                     DexFile::kAllRaw)) {
    return false;
  }
  if (annotation_value.type_ != DexFile::kDexAnnotationInt) {
    return false;
  }
  *flags = annotation_value.value_.GetI();
  return true;
}

ObjPtr<mirror::ObjectArray<mirror::String>> GetSignatureAnnotationForClass(
    Handle<mirror::Class> klass) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  return GetSignatureValue(data, annotation_set);
}

const char* GetSourceDebugExtension(Handle<mirror::Class> klass) {
  // Before instantiating ClassData, check that klass has a DexCache
  // assigned.  The ClassData constructor indirectly dereferences it
  // when calling klass->GetDexFile().
  if (klass->GetDexCache() == nullptr) {
    DCHECK(klass->IsPrimitive() || klass->IsArrayClass());
    return nullptr;
  }

  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }

  const AnnotationItem* annotation_item = SearchAnnotationSet(
      data.GetDexFile(),
      annotation_set,
      "Ldalvik/annotation/SourceDebugExtension;",
      DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }

  const uint8_t* annotation =
      SearchEncodedAnnotation(data.GetDexFile(), annotation_item->annotation_, "value");
  if (annotation == nullptr) {
    return nullptr;
  }
  DexFile::AnnotationValue annotation_value;
  if (!ProcessAnnotationValue<false>(data,
                                     &annotation,
                                     &annotation_value,
                                     ScopedNullHandle<mirror::Class>(),
                                     DexFile::kAllRaw)) {
    return nullptr;
  }
  if (annotation_value.type_ != DexFile::kDexAnnotationString) {
    return nullptr;
  }
  dex::StringIndex index(static_cast<uint32_t>(annotation_value.value_.GetI()));
  return data.GetDexFile().GetStringData(index);
}

ObjPtr<mirror::Class> GetNestHost(Handle<mirror::Class> klass) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(data.GetDexFile(), annotation_set, "Ldalvik/annotation/NestHost;",
                          DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  ObjPtr<mirror::Object> obj = GetAnnotationValue(data,
                                                  annotation_item,
                                                  "host",
                                                  ScopedNullHandle<mirror::Class>(),
                                                  DexFile::kDexAnnotationType);
  if (obj == nullptr) {
    return nullptr;
  }
  if (!obj->IsClass()) {
    // TypeNotPresentException, throw the NoClassDefFoundError.
    Thread::Current()->SetException(obj->AsThrowable()->GetCause());
    return nullptr;
  }
  return obj->AsClass();
}

ObjPtr<mirror::ObjectArray<mirror::Class>> GetNestMembers(Handle<mirror::Class> klass) {
  return GetAnnotationArrayValue<mirror::Class>(klass,
                                                "Ldalvik/annotation/NestMembers;",
                                                "classes");
}

ObjPtr<mirror::ObjectArray<mirror::Class>> GetPermittedSubclasses(Handle<mirror::Class> klass) {
  return GetAnnotationArrayValue<mirror::Class>(klass,
                                                "Ldalvik/annotation/PermittedSubclasses;",
                                                "value");
}

ObjPtr<mirror::Object> getRecordAnnotationElement(Handle<mirror::Class> klass,
                                                  Handle<mirror::Class> array_class,
                                                  const char* element_name) {
  ClassData data(klass);
  const DexFile& dex_file = klass->GetDexFile();
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(
      dex_file, annotation_set, "Ldalvik/annotation/Record;", DexFile::kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  const uint8_t* annotation =
      SearchEncodedAnnotation(dex_file, annotation_item->annotation_, element_name);
  if (annotation == nullptr) {
    return nullptr;
  }
  DexFile::AnnotationValue annotation_value;
  bool result = Runtime::Current()->IsActiveTransaction()
      ? ProcessAnnotationValue<true>(data,
                                     &annotation,
                                     &annotation_value,
                                     array_class,
                                     DexFile::kPrimitivesOrObjects)
      : ProcessAnnotationValue<false>(data,
                                      &annotation,
                                      &annotation_value,
                                      array_class,
                                      DexFile::kPrimitivesOrObjects);
  if (!result) {
    return nullptr;
  }
  return annotation_value.value_.GetL();
}

bool IsClassAnnotationPresent(Handle<mirror::Class> klass, Handle<mirror::Class> annotation_class) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = GetAnnotationItemFromAnnotationSet(
      data, annotation_set, DexFile::kDexVisibilityRuntime, annotation_class);
  return annotation_item != nullptr;
}

int32_t GetLineNumFromPC(const DexFile* dex_file, ArtMethod* method, uint32_t rel_pc) {
  // For native method, lineno should be -2 to indicate it is native. Note that
  // "line number == -2" is how libcore tells from StackTraceElement.
  if (!method->HasCodeItem()) {
    return -2;
  }

  CodeItemDebugInfoAccessor accessor(method->DexInstructionDebugInfo());
  DCHECK(accessor.HasCodeItem()) << method->PrettyMethod() << " " << dex_file->GetLocation();

  // A method with no line number info should return -1
  uint32_t line_num = -1;
  accessor.GetLineNumForPc(rel_pc, &line_num);
  return line_num;
}

template<bool kTransactionActive>
void RuntimeEncodedStaticFieldValueIterator::ReadValueToField(ArtField* field) const {
  DCHECK(dex_cache_ != nullptr);
  switch (type_) {
    case kBoolean: field->SetBoolean<kTransactionActive>(field->GetDeclaringClass(), jval_.z);
        break;
    case kByte:    field->SetByte<kTransactionActive>(field->GetDeclaringClass(), jval_.b); break;
    case kShort:   field->SetShort<kTransactionActive>(field->GetDeclaringClass(), jval_.s); break;
    case kChar:    field->SetChar<kTransactionActive>(field->GetDeclaringClass(), jval_.c); break;
    case kInt:     field->SetInt<kTransactionActive>(field->GetDeclaringClass(), jval_.i); break;
    case kLong:    field->SetLong<kTransactionActive>(field->GetDeclaringClass(), jval_.j); break;
    case kFloat:   field->SetFloat<kTransactionActive>(field->GetDeclaringClass(), jval_.f); break;
    case kDouble:  field->SetDouble<kTransactionActive>(field->GetDeclaringClass(), jval_.d); break;
    case kNull:    field->SetObject<kTransactionActive>(field->GetDeclaringClass(), nullptr); break;
    case kString: {
      ObjPtr<mirror::String> resolved = linker_->ResolveString(dex::StringIndex(jval_.i),
                                                               dex_cache_);
      field->SetObject<kTransactionActive>(field->GetDeclaringClass(), resolved);
      break;
    }
    case kType: {
      ObjPtr<mirror::Class> resolved = linker_->ResolveType(dex::TypeIndex(jval_.i),
                                                            dex_cache_,
                                                            class_loader_);
      field->SetObject<kTransactionActive>(field->GetDeclaringClass(), resolved);
      break;
    }
    default: UNIMPLEMENTED(FATAL) << ": type " << type_;
  }
}
template
void RuntimeEncodedStaticFieldValueIterator::ReadValueToField<true>(ArtField* field) const;
template
void RuntimeEncodedStaticFieldValueIterator::ReadValueToField<false>(ArtField* field) const;

inline static VisitorStatus VisitElement(AnnotationVisitor* visitor,
                                         const char* element_name,
                                         uint8_t depth,
                                         uint32_t element_index,
                                         const DexFile::AnnotationValue& annotation_value)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (depth == 0) {
    return visitor->VisitAnnotationElement(
        element_name, annotation_value.type_, annotation_value.value_);
  } else {
    return visitor->VisitArrayElement(
        depth - 1, element_index, annotation_value.type_, annotation_value.value_);
  }
}

static VisitorStatus VisitEncodedValue(const ClassData& klass,
                                       const DexFile& dex_file,
                                       const uint8_t** annotation_ptr,
                                       AnnotationVisitor* visitor,
                                       const char* element_name,
                                       uint8_t depth,
                                       uint32_t element_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DexFile::AnnotationValue annotation_value;
  // kTransactionActive is safe because the result_style is kAllRaw.
  bool is_consumed = ProcessAnnotationValue<false>(klass,
                                                   annotation_ptr,
                                                   &annotation_value,
                                                   ScopedNullHandle<mirror::Class>(),
                                                   DexFile::kAllRaw);

  VisitorStatus status =
      VisitElement(visitor, element_name, depth, element_index, annotation_value);
  switch (annotation_value.type_) {
    case DexFile::kDexAnnotationArray: {
      DCHECK(!is_consumed) << " unexpected consumption of array-typed element '" << element_name
                           << "' annotating the class " << klass.GetRealClass()->PrettyClass();
      SkipEncodedValueHeaderByte(annotation_ptr);
      uint32_t array_size = DecodeUnsignedLeb128(annotation_ptr);
      uint8_t next_depth = depth + 1;
      VisitorStatus element_status = (status == VisitorStatus::kVisitInner) ?
                                         VisitorStatus::kVisitNext :
                                         VisitorStatus::kVisitBreak;
      uint32_t i = 0;
      for (; i < array_size && element_status != VisitorStatus::kVisitBreak; ++i) {
        element_status = VisitEncodedValue(
            klass, dex_file, annotation_ptr, visitor, element_name, next_depth, i);
      }
      for (; i < array_size; ++i) {
        SkipAnnotationValue(dex_file, annotation_ptr);
      }
      break;
    }
    case DexFile::kDexAnnotationAnnotation: {
      DCHECK(!is_consumed) << " unexpected consumption of annotation-typed element '"
                           << element_name << "' annotating the class "
                           << klass.GetRealClass()->PrettyClass();
      SkipEncodedValueHeaderByte(annotation_ptr);
      DecodeUnsignedLeb128(annotation_ptr);  // unused type_index
      uint32_t size = DecodeUnsignedLeb128(annotation_ptr);
      for (; size != 0u; --size) {
        DecodeUnsignedLeb128(annotation_ptr);  // unused element_name_index
        SkipAnnotationValue(dex_file, annotation_ptr);
      }
      break;
    }
    default: {
      // kDexAnnotationArray and kDexAnnotationAnnotation are the only 2 known value_types causing
      // ProcessAnnotationValue return false. For other value_types, we shouldn't need to iterate
      // over annotation_ptr and skip the value here.
      DCHECK(is_consumed) << StringPrintf(
          "consumed annotation element type 0x%02x of %s for the class %s",
          annotation_value.type_,
          element_name,
          klass.GetRealClass()->PrettyClass().c_str());
      if (UNLIKELY(!is_consumed)) {
        SkipAnnotationValue(dex_file, annotation_ptr);
      }
      break;
    }
  }

  return status;
}

void VisitClassAnnotations(Handle<mirror::Class> klass, AnnotationVisitor* visitor) {
  ClassData data(klass);
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(data);
  if (annotation_set == nullptr) {
    return;
  }

  const DexFile& dex_file = data.GetDexFile();
  for (uint32_t i = 0; i < annotation_set->size_; ++i) {
    const AnnotationItem* annotation_item = dex_file.GetAnnotationItem(annotation_set, i);
    uint8_t visibility = annotation_item->visibility_;
    const uint8_t* annotation = annotation_item->annotation_;
    uint32_t type_index = DecodeUnsignedLeb128(&annotation);
    const char* annotation_descriptor = dex_file.GetTypeDescriptor(dex::TypeIndex(type_index));
    VisitorStatus status = visitor->VisitAnnotation(annotation_descriptor, visibility);
    switch (status) {
      case VisitorStatus::kVisitBreak:
        return;
      case VisitorStatus::kVisitNext:
        continue;
      case VisitorStatus::kVisitInner:
        // Visit the annotation elements
        break;
    }

    uint32_t size = DecodeUnsignedLeb128(&annotation);
    while (size != 0) {
      uint32_t element_name_index = DecodeUnsignedLeb128(&annotation);
      const char* element_name =
          dex_file.GetStringData(dex_file.GetStringId(dex::StringIndex(element_name_index)));

      status = VisitEncodedValue(
          data, dex_file, &annotation, visitor, element_name, /*depth=*/0, /*ignored*/ 0);
      if (status == VisitorStatus::kVisitBreak) {
        break;
      }
      size--;
    }
  }
}

}  // namespace annotations

}  // namespace art
