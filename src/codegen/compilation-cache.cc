// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/compilation-cache.h"

#include "src/codegen/script-details.h"
#include "src/common/globals.h"
#include "src/heap/factory.h"
#include "src/logging/counters.h"
#include "src/logging/log.h"
#include "src/objects/compilation-cache-table-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/slots.h"
#include "src/objects/visitors.h"
#include "src/utils/ostreams.h"

namespace v8 {
namespace internal {

// Initial size of each compilation cache table allocated.
static const int kInitialCacheSize = 64;

CompilationCache::CompilationCache(Isolate* isolate)
    : isolate_(isolate),
      script_(isolate),
      eval_global_(isolate),
      eval_contextual_(isolate),
      reg_exp_(isolate),
      enabled_script_and_eval_(true) {}

Handle<CompilationCacheTable> CompilationCacheEvalOrScript::GetTable() {
  if (table_.IsUndefined(isolate())) {
    return CompilationCacheTable::New(isolate(), kInitialCacheSize);
  }
  return handle(CompilationCacheTable::cast(table_), isolate());
}

Handle<CompilationCacheTable> CompilationCacheRegExp::GetTable(int generation) {
  DCHECK_LT(generation, kGenerations);
  Handle<CompilationCacheTable> result;
  if (tables_[generation].IsUndefined(isolate())) {
    result = CompilationCacheTable::New(isolate(), kInitialCacheSize);
    tables_[generation] = *result;
  } else {
    CompilationCacheTable table =
        CompilationCacheTable::cast(tables_[generation]);
    result = Handle<CompilationCacheTable>(table, isolate());
  }
  return result;
}

void CompilationCacheRegExp::Age() {
  static_assert(kGenerations > 1);

  // Age the generations implicitly killing off the oldest.
  for (int i = kGenerations - 1; i > 0; i--) {
    tables_[i] = tables_[i - 1];
  }

  // Set the first generation as unborn.
  tables_[0] = ReadOnlyRoots(isolate()).undefined_value();
}

void CompilationCacheScript::Age() {
  DisallowGarbageCollection no_gc;
  if (!FLAG_isolate_script_cache_ageing) return;
  if (table_.IsUndefined(isolate())) return;
  CompilationCacheTable table = CompilationCacheTable::cast(table_);

  for (InternalIndex entry : table.IterateEntries()) {
    Object key;
    if (!table.ToKey(isolate(), entry, &key)) continue;
    DCHECK(key.IsFixedArray());

    Object value = table.PrimaryValueAt(entry);
    if (!value.IsUndefined(isolate())) {
      SharedFunctionInfo info = SharedFunctionInfo::cast(value);
      if (info.HasBytecodeArray() && info.GetBytecodeArray(isolate()).IsOld()) {
        table.RemoveEntry(entry);
      }
    }
  }
}

void CompilationCacheEval::Age() {
  DisallowGarbageCollection no_gc;
  if (table_.IsUndefined(isolate())) return;
  CompilationCacheTable table = CompilationCacheTable::cast(table_);

  for (InternalIndex entry : table.IterateEntries()) {
    Object key;
    if (!table.ToKey(isolate(), entry, &key)) continue;

    if (key.IsNumber(isolate())) {
      // The ageing mechanism for the initial dummy entry in the eval cache.
      // The 'key' is the hash represented as a Number. The 'value' is a smi
      // counting down from kHashGenerations. On reaching zero, the entry is
      // cleared.
      // Note: The following static assert only establishes an explicit
      // connection between initialization- and use-sites of the smi value
      // field.
      static_assert(CompilationCacheTable::kHashGenerations);
      const int new_count = Smi::ToInt(table.PrimaryValueAt(entry)) - 1;
      if (new_count == 0) {
        table.RemoveEntry(entry);
      } else {
        DCHECK_GT(new_count, 0);
        table.SetPrimaryValueAt(entry, Smi::FromInt(new_count),
                                SKIP_WRITE_BARRIER);
      }
    } else {
      DCHECK(key.IsFixedArray());
      // The ageing mechanism for eval caches.
      SharedFunctionInfo info =
          SharedFunctionInfo::cast(table.PrimaryValueAt(entry));
      if (info.HasBytecodeArray() && info.GetBytecodeArray(isolate()).IsOld()) {
        table.RemoveEntry(entry);
      }
    }
  }
}

void CompilationCacheEvalOrScript::Iterate(RootVisitor* v) {
  v->VisitRootPointer(Root::kCompilationCache, nullptr,
                      FullObjectSlot(&table_));
}

void CompilationCacheRegExp::Iterate(RootVisitor* v) {
  v->VisitRootPointers(Root::kCompilationCache, nullptr,
                       FullObjectSlot(&tables_[0]),
                       FullObjectSlot(&tables_[kGenerations]));
}

void CompilationCacheEvalOrScript::Clear() {
  table_ = ReadOnlyRoots(isolate()).undefined_value();
}

void CompilationCacheRegExp::Clear() {
  MemsetPointer(reinterpret_cast<Address*>(tables_),
                ReadOnlyRoots(isolate()).undefined_value().ptr(), kGenerations);
}

void CompilationCacheEvalOrScript::Remove(
    Handle<SharedFunctionInfo> function_info) {
  if (table_.IsUndefined(isolate())) return;
  CompilationCacheTable::cast(table_).Remove(*function_info);
}

namespace {

// We only re-use a cached function for some script source code if the
// script originates from the same place. This is to avoid issues
// when reporting errors, etc.
bool HasOrigin(Isolate* isolate, Handle<SharedFunctionInfo> function_info,
               const ScriptDetails& script_details) {
  Handle<Script> script =
      Handle<Script>(Script::cast(function_info->script()), isolate);
  // If the script name isn't set, the boilerplate script should have
  // an undefined name to have the same origin.
  Handle<Object> name;
  if (!script_details.name_obj.ToHandle(&name)) {
    return script->name().IsUndefined(isolate);
  }
  // Do the fast bailout checks first.
  if (script_details.line_offset != script->line_offset()) return false;
  if (script_details.column_offset != script->column_offset()) return false;
  // Check that both names are strings. If not, no match.
  if (!name->IsString() || !script->name().IsString()) return false;
  // Are the origin_options same?
  if (script_details.origin_options.Flags() !=
      script->origin_options().Flags()) {
    return false;
  }
  // Compare the two name strings for equality.
  if (!String::Equals(isolate, Handle<String>::cast(name),
                      Handle<String>(String::cast(script->name()), isolate))) {
    return false;
  }

  // TODO(cbruni, chromium:1244145): Remove once migrated to the context
  Handle<Object> maybe_host_defined_options;
  if (!script_details.host_defined_options.ToHandle(
          &maybe_host_defined_options)) {
    maybe_host_defined_options = isolate->factory()->empty_fixed_array();
  }
  Handle<FixedArray> host_defined_options =
      Handle<FixedArray>::cast(maybe_host_defined_options);
  Handle<FixedArray> script_options(
      FixedArray::cast(script->host_defined_options()), isolate);
  int length = host_defined_options->length();
  if (length != script_options->length()) return false;

  for (int i = 0; i < length; i++) {
    // host-defined options is a v8::PrimitiveArray.
    DCHECK(host_defined_options->get(i).IsPrimitive());
    DCHECK(script_options->get(i).IsPrimitive());
    if (!host_defined_options->get(i).StrictEquals(script_options->get(i))) {
      return false;
    }
  }
  return true;
}
}  // namespace

// TODO(245): Need to allow identical code from different contexts to
// be cached in the same script generation. Currently the first use
// will be cached, but subsequent code from different source / line
// won't.
MaybeHandle<SharedFunctionInfo> CompilationCacheScript::Lookup(
    Handle<String> source, const ScriptDetails& script_details,
    LanguageMode language_mode) {
  MaybeHandle<SharedFunctionInfo> result;

  // Probe the script generation tables. Make sure not to leak handles
  // into the caller's handle scope.
  {
    HandleScope scope(isolate());
    Handle<CompilationCacheTable> table = GetTable();
    MaybeHandle<SharedFunctionInfo> probe = CompilationCacheTable::LookupScript(
        table, source, language_mode, isolate());
    Handle<SharedFunctionInfo> function_info;
    if (probe.ToHandle(&function_info)) {
      // Break when we've found a suitable shared function info that
      // matches the origin.
      if (HasOrigin(isolate(), function_info, script_details)) {
        result = scope.CloseAndEscape(function_info);
      }
    }
  }

  // Once outside the manacles of the handle scope, we need to recheck
  // to see if we actually found a cached script. If so, we return a
  // handle created in the caller's handle scope.
  Handle<SharedFunctionInfo> function_info;
  if (result.ToHandle(&function_info)) {
    // Since HasOrigin can allocate, we need to protect the SharedFunctionInfo
    // with handles during the call.
    DCHECK(HasOrigin(isolate(), function_info, script_details));
    isolate()->counters()->compilation_cache_hits()->Increment();
    LOG(isolate(), CompilationCacheEvent("hit", "script", *function_info));
  } else {
    isolate()->counters()->compilation_cache_misses()->Increment();
  }
  return result;
}

void CompilationCacheScript::Put(Handle<String> source,
                                 LanguageMode language_mode,
                                 Handle<SharedFunctionInfo> function_info) {
  HandleScope scope(isolate());
  Handle<CompilationCacheTable> table = GetTable();
  table_ = *CompilationCacheTable::PutScript(table, source, language_mode,
                                             function_info, isolate());
}

InfoCellPair CompilationCacheEval::Lookup(Handle<String> source,
                                          Handle<SharedFunctionInfo> outer_info,
                                          Handle<Context> native_context,
                                          LanguageMode language_mode,
                                          int position) {
  HandleScope scope(isolate());
  // Make sure not to leak the table into the surrounding handle
  // scope. Otherwise, we risk keeping old tables around even after
  // having cleared the cache.
  InfoCellPair result;
  Handle<CompilationCacheTable> table = GetTable();
  result = CompilationCacheTable::LookupEval(
      table, source, outer_info, native_context, language_mode, position);
  if (result.has_shared()) {
    isolate()->counters()->compilation_cache_hits()->Increment();
  } else {
    isolate()->counters()->compilation_cache_misses()->Increment();
  }
  return result;
}

void CompilationCacheEval::Put(Handle<String> source,
                               Handle<SharedFunctionInfo> outer_info,
                               Handle<SharedFunctionInfo> function_info,
                               Handle<Context> native_context,
                               Handle<FeedbackCell> feedback_cell,
                               int position) {
  HandleScope scope(isolate());
  Handle<CompilationCacheTable> table = GetTable();
  table_ =
      *CompilationCacheTable::PutEval(table, source, outer_info, function_info,
                                      native_context, feedback_cell, position);
}

MaybeHandle<FixedArray> CompilationCacheRegExp::Lookup(Handle<String> source,
                                                       JSRegExp::Flags flags) {
  HandleScope scope(isolate());
  // Make sure not to leak the table into the surrounding handle
  // scope. Otherwise, we risk keeping old tables around even after
  // having cleared the cache.
  Handle<Object> result = isolate()->factory()->undefined_value();
  int generation;
  for (generation = 0; generation < kGenerations; generation++) {
    Handle<CompilationCacheTable> table = GetTable(generation);
    result = table->LookupRegExp(source, flags);
    if (result->IsFixedArray()) break;
  }
  if (result->IsFixedArray()) {
    Handle<FixedArray> data = Handle<FixedArray>::cast(result);
    if (generation != 0) {
      Put(source, flags, data);
    }
    isolate()->counters()->compilation_cache_hits()->Increment();
    return scope.CloseAndEscape(data);
  } else {
    isolate()->counters()->compilation_cache_misses()->Increment();
    return MaybeHandle<FixedArray>();
  }
}

void CompilationCacheRegExp::Put(Handle<String> source, JSRegExp::Flags flags,
                                 Handle<FixedArray> data) {
  HandleScope scope(isolate());
  Handle<CompilationCacheTable> table = GetTable(0);
  tables_[0] =
      *CompilationCacheTable::PutRegExp(isolate(), table, source, flags, data);
}

void CompilationCache::Remove(Handle<SharedFunctionInfo> function_info) {
  if (!IsEnabledScriptAndEval()) return;

  eval_global_.Remove(function_info);
  eval_contextual_.Remove(function_info);
  script_.Remove(function_info);
}

MaybeHandle<SharedFunctionInfo> CompilationCache::LookupScript(
    Handle<String> source, const ScriptDetails& script_details,
    LanguageMode language_mode) {
  if (!IsEnabledScriptAndEval()) return MaybeHandle<SharedFunctionInfo>();
  return script_.Lookup(source, script_details, language_mode);
}

InfoCellPair CompilationCache::LookupEval(Handle<String> source,
                                          Handle<SharedFunctionInfo> outer_info,
                                          Handle<Context> context,
                                          LanguageMode language_mode,
                                          int position) {
  InfoCellPair result;
  if (!IsEnabledScriptAndEval()) return result;

  const char* cache_type;

  if (context->IsNativeContext()) {
    result = eval_global_.Lookup(source, outer_info, context, language_mode,
                                 position);
    cache_type = "eval-global";

  } else {
    DCHECK_NE(position, kNoSourcePosition);
    Handle<Context> native_context(context->native_context(), isolate());
    result = eval_contextual_.Lookup(source, outer_info, native_context,
                                     language_mode, position);
    cache_type = "eval-contextual";
  }

  if (result.has_shared()) {
    LOG(isolate(), CompilationCacheEvent("hit", cache_type, result.shared()));
  }

  return result;
}

MaybeHandle<FixedArray> CompilationCache::LookupRegExp(Handle<String> source,
                                                       JSRegExp::Flags flags) {
  return reg_exp_.Lookup(source, flags);
}

void CompilationCache::PutScript(Handle<String> source,
                                 LanguageMode language_mode,
                                 Handle<SharedFunctionInfo> function_info) {
  if (!IsEnabledScriptAndEval()) return;
  LOG(isolate(), CompilationCacheEvent("put", "script", *function_info));

  script_.Put(source, language_mode, function_info);
}

void CompilationCache::PutEval(Handle<String> source,
                               Handle<SharedFunctionInfo> outer_info,
                               Handle<Context> context,
                               Handle<SharedFunctionInfo> function_info,
                               Handle<FeedbackCell> feedback_cell,
                               int position) {
  if (!IsEnabledScriptAndEval()) return;

  const char* cache_type;
  HandleScope scope(isolate());
  if (context->IsNativeContext()) {
    eval_global_.Put(source, outer_info, function_info, context, feedback_cell,
                     position);
    cache_type = "eval-global";
  } else {
    DCHECK_NE(position, kNoSourcePosition);
    Handle<Context> native_context(context->native_context(), isolate());
    eval_contextual_.Put(source, outer_info, function_info, native_context,
                         feedback_cell, position);
    cache_type = "eval-contextual";
  }
  LOG(isolate(), CompilationCacheEvent("put", cache_type, *function_info));
}

void CompilationCache::PutRegExp(Handle<String> source, JSRegExp::Flags flags,
                                 Handle<FixedArray> data) {
  reg_exp_.Put(source, flags, data);
}

void CompilationCache::Clear() {
  script_.Clear();
  eval_global_.Clear();
  eval_contextual_.Clear();
  reg_exp_.Clear();
}

void CompilationCache::Iterate(RootVisitor* v) {
  script_.Iterate(v);
  eval_global_.Iterate(v);
  eval_contextual_.Iterate(v);
  reg_exp_.Iterate(v);
}

void CompilationCache::MarkCompactPrologue() {
  script_.Age();
  eval_global_.Age();
  eval_contextual_.Age();
  reg_exp_.Age();
}

void CompilationCache::EnableScriptAndEval() {
  enabled_script_and_eval_ = true;
}

void CompilationCache::DisableScriptAndEval() {
  enabled_script_and_eval_ = false;
  Clear();
}

}  // namespace internal
}  // namespace v8
