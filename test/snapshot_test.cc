#include <iostream>
#include "gtest/gtest.h"
#include "v8.h"
#include "libplatform/libplatform.h"
#include "src/execution/isolate-inl.h"

using namespace v8;

void print_data(StartupData* startup_data) {
  size_t size = static_cast<size_t>(startup_data->raw_size);
  for (size_t i = 0; i < size; i++) {
    char endchar = i != size - 1 ? ',' : '\n';
    std::cout << std::to_string(startup_data->data[i]) << endchar;
  }
}

extern void _v8_internal_Print_Object(void* object);

template<typename T>
static void print_local(v8::Local<T> obj) {
  _v8_internal_Print_Object(*((v8::internal::Object**)*obj));
}

class SnapshotTest : public ::testing::Test {
 public:
 protected:
  static std::unique_ptr<Platform> platform_;
  static void SetUpTestCase() {
    platform_ = platform::NewDefaultPlatform();
    V8::SetFlagsFromString("--random_seed=42");
    V8::InitializePlatform(platform_.get());
    V8::Initialize();
  }
  static void TearDownTestCase() {
    V8::ShutdownPlatform();
  }
};

std::unique_ptr<Platform> SnapshotTest::platform_;

static void SomeExternalFunction(const FunctionCallbackInfo<Value>& args) {
    std::cout << "SomeExternalFunction...\n";
}

TEST_F(SnapshotTest, CreateSnapshot) {
  StartupData startup_data;
  size_t index;

  std::vector<intptr_t> external_references = { reinterpret_cast<intptr_t>(nullptr)};
  {
    Isolate* isolate = Isolate::Allocate();
    // Create a new SnapshotCreator and notice that we are passing in the pointer
    // to the external_references which only contains one function address in
    // our case.
    SnapshotCreator snapshot_creator(isolate, external_references.data());
    {
      HandleScope scope(isolate);
      snapshot_creator.SetDefaultContext(Context::New(isolate));
      Local<Context> context = Context::New(isolate);
      {
        Context::Scope context_scope(context);
        TryCatch try_catch(isolate);

        // Add the following function to the context
        const char* js = R"(function test_snapshot() { 
          return 'from test_snapshot function';
        })";
        Local<String> src = String::NewFromUtf8(isolate, js).ToLocalChecked();
        ScriptOrigin origin(String::NewFromUtf8Literal(isolate, "function"));
        ScriptCompiler::Source source(src, origin);                     
        Local<Script> script;
        EXPECT_TRUE(ScriptCompiler::Compile(context, &source).ToLocal(&script));
        EXPECT_FALSE(script->Run(context).ToLocalChecked().IsEmpty());
        EXPECT_FALSE(try_catch.HasCaught());
      }

      index = snapshot_creator.AddContext(context);
      std::cout << "context index: " << index << '\n';
    }
    startup_data = snapshot_creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kKeep);
    std::cout << "size of blob: " << startup_data.raw_size << '\n';
  }
  // SnapshotCreators destructor will call Isolate::Exit and Isolate::Dispose
  // Next, create a new Isolate and then a new Context using the snapshot.

  Isolate::CreateParams create_params;
  // Specify the startup_data blob created earlier.
  create_params.snapshot_blob = &startup_data;
  create_params.array_buffer_allocator = ArrayBuffer::Allocator::NewDefaultAllocator();
  Isolate* isolate = Isolate::New(create_params);
  {
    HandleScope scope(isolate);
    // Create the Context from the snapshot index.
    Local<Context> context = Context::FromSnapshot(isolate, index).ToLocalChecked();
    Context::Scope context_scope(context);
    TryCatch try_catch(isolate);
    // Add JavaScript code that calls the function we added previously.
    const char* js = "test_snapshot();";
    Local<String> src = String::NewFromUtf8(isolate, js).ToLocalChecked();
    ScriptOrigin origin(String::NewFromUtf8Literal(isolate, "usage"));
    ScriptCompiler::Source source(src, origin);                     
    Local<Script> script;
    EXPECT_TRUE(ScriptCompiler::Compile(context, &source).ToLocal(&script));
    MaybeLocal<Value> maybe_result = script->Run(context);
    EXPECT_FALSE(maybe_result.IsEmpty());

    Local<Value> result = maybe_result.ToLocalChecked();
    String::Utf8Value utf8(isolate, result);
    EXPECT_STREQ("from test_snapshot function", *utf8);
    EXPECT_FALSE(try_catch.HasCaught());
  }
  isolate->Dispose();
}

class ExternalData {
 public:
  ExternalData(int x) : x_(x) {}
  int x() { return x_; }
 private:
  int x_;
};

TEST_F(SnapshotTest, CreateSnapshotWithData) {
  StartupData startup_data;
  size_t index;
  size_t data_index;

  std::vector<intptr_t> external_references = { reinterpret_cast<intptr_t>(nullptr)};
  {
    Isolate* isolate = nullptr;
    isolate = Isolate::Allocate();
    SnapshotCreator snapshot_creator(isolate, external_references.data());
    {
      HandleScope scope(isolate);
      snapshot_creator.SetDefaultContext(Context::New(isolate));
      Local<Context> context = Context::New(isolate);
      Local<Number> data = Number::New(isolate, 18);
      data_index = snapshot_creator.AddData(context, data);
      std::cout << "data_index: " << data_index << '\n';
      index = snapshot_creator.AddContext(context);
      std::cout << "context index: " << index << '\n';
    }
    startup_data = snapshot_creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kKeep);
    std::cout << "size of blob: " << startup_data.raw_size << '\n';
  }

  Isolate::CreateParams create_params;
  // Specify the startup_data blob created earlier.
  create_params.snapshot_blob = &startup_data;
  create_params.array_buffer_allocator = ArrayBuffer::Allocator::NewDefaultAllocator();
  Isolate* isolate = Isolate::New(create_params);
  {
    HandleScope scope(isolate);
    // Create the Context from the snapshot index.
    Local<Context> context = Context::FromSnapshot(isolate, index).ToLocalChecked();
    Context::Scope context_scope(context);
    MaybeLocal<Object> obj = context->GetDataFromSnapshotOnce<Object>(data_index);
    EXPECT_FALSE(obj.IsEmpty());
    Local<Object> o = obj.ToLocalChecked();
    Local<Number> nr = o.As<Number>();
    EXPECT_EQ(18, nr->Value());
  }
  isolate->Dispose();
}


void ExternalRefFunction(const FunctionCallbackInfo<Value>& args) {
    String::Utf8Value str(args.GetIsolate(), args[0]);
    printf("ExternalRefFunction argument = %s\n", *str);
    args.GetReturnValue().Set(
        String::NewFromUtf8Literal(args.GetIsolate(),
          "ExternalRefFunction done."));
}

void ExternalRefFunction2(const FunctionCallbackInfo<Value>& args) {
    String::Utf8Value str(args.GetIsolate(), args[0]);
    printf("ExternalRefFunction2 argument = %s\n", *str);
    args.GetReturnValue().Set(
        String::NewFromUtf8Literal(args.GetIsolate(),
          "ExternalRefFunction2 done."));
}

TEST_F(SnapshotTest, ExternalReference) {
  StartupData startup_data;
  size_t index;

  std::vector<intptr_t> external_refs;
  std::cout << "address of ExternalRefFunction function: " << 
    reinterpret_cast<intptr_t>(ExternalRefFunction) << '\n';
  external_refs.push_back(reinterpret_cast<intptr_t>(ExternalRefFunction));

  std::cout << "external_refs: ";
  for (std::vector<intptr_t>::const_iterator i = external_refs.begin(); i != external_refs.end(); ++i) {
    std::cout << *i << ' ';
  }
  std::cout << '\n';

  {
    Isolate* isolate = nullptr;
    isolate = Isolate::Allocate();
    SnapshotCreator snapshot_creator(isolate, external_refs.data());
    {
      HandleScope scope(isolate);
      snapshot_creator.SetDefaultContext(Context::New(isolate));
      Local<Context> context = Context::New(isolate);
      {
        Context::Scope context_scope(context);
        TryCatch try_catch(isolate);

        Local<Function> function = FunctionTemplate::New(isolate,
            ExternalRefFunction, Local<Value>(), Local<Signature>(), 0, 
            ConstructorBehavior::kThrow,
            SideEffectType::kHasSideEffect)->GetFunction(context).ToLocalChecked();


        Local<String> func_name = String::NewFromUtf8Literal(isolate, "external");
        context->Global()->Set(context, func_name, function).Check();
        function->SetName(func_name);

        v8::internal::HeapObject heap_obj = v8::internal::HeapObject::cast(*((v8::internal::Object*)*function));
        print_local(function);
      }

      index = snapshot_creator.AddContext(context);
    }
    startup_data = snapshot_creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kKeep);
  }

  Isolate::CreateParams create_params;
  // Use the startup_data (the snapshot blob created above)
  create_params.snapshot_blob = &startup_data;
  // Add the external references to functions 
  std::vector<intptr_t> external_refs2;
  std::cout << "address of ExternalRefFunction2 function: " << 
    reinterpret_cast<intptr_t>(ExternalRefFunction2) << '\n';
  external_refs2.push_back(reinterpret_cast<intptr_t>(ExternalRefFunction2));
  create_params.external_references = external_refs2.data();

  create_params.array_buffer_allocator = ArrayBuffer::Allocator::NewDefaultAllocator();

  Isolate* isolate = Isolate::New(create_params);
  /*
  v8::internal::Isolate* i_isolate = reinterpret_cast<v8::internal::Isolate*>(isolate);
  v8::internal::ExternalReferenceTable* ert = i_isolate->external_reference_table();
  const char* name = ert->ResolveSymbol((void*)reinterpret_cast<intptr_t>(ExternalRefFunction2));
  std::cout << "ResoleSymbol ExternalRefFunction2: " << name << '\n';
  */
  {
    HandleScope scope(isolate);
    Local<Context> context = Context::FromSnapshot(isolate, index).ToLocalChecked();
    {
      Context::Scope context_scope(context);
      TryCatch try_catch(isolate);
      Local<String> src = String::NewFromUtf8Literal(isolate, "external('some arg');");
      ScriptOrigin origin(String::NewFromUtf8Literal(isolate, "function"));
      ScriptCompiler::Source source(src, origin);
      Local<Script> script;
      EXPECT_TRUE(ScriptCompiler::Compile(context, &source).ToLocal(&script));
      MaybeLocal<Value> maybe_result = script->Run(context);
      EXPECT_FALSE(maybe_result.IsEmpty());
      Local<Value> result = maybe_result.ToLocalChecked();
      String::Utf8Value utf8(isolate, result);
      EXPECT_STREQ("ExternalRefFunction2 done.", *utf8);
      EXPECT_FALSE(try_catch.HasCaught());
    }
  }
  isolate->Dispose();
}



void internal_function() {
  std::cout << "internal_function..." << '\n';
}

void Constructor(const FunctionCallbackInfo<Value>& args) {
  std::cout << "Constructor..." << '\n';
  args.Holder()->SetAlignedPointerInInternalField(0, reinterpret_cast<void*>(internal_function));
}

class Something {
 public:
   Something(const char* s) : value_(s) {}
   const char* value() { return value_; }
 private:
   const char* value_;
};

/*
 * This is part of the snapshot serialization process, taking in-memory objects
 * and adding them to the StartupData to be written to storage.
 */
StartupData SerializeInternalFields(Local<Object> holder,
                                    int index,
                                    void* data) {
  Something* s = static_cast<Something*>(holder->GetAlignedPointerFromInternalField(index));
  std::cout << "SerializeInternalFields index: " << index
            << ", value: " << s->value() << '\n';


  int size = sizeof(*s);
  char* payload = new char[size];
  memcpy(payload, s, size);
  std::cout << "SerializeInternalFields payload size: " << size << '\n';
  return {payload, size};
}

/*
 * This is part of the snapshot deserialization process, this function is passed
 * the payload data the was stored in the serialization process. This raw data
 * should now be put into a new Something instance and set on the passed in
 * holder object using the index passed in.
 */
void DeserializeInternalFields(Local<Object> holder,
                               int index,
                               StartupData payload,
                               void* data) {
  std::cout << "DeserializeInternalFields payload size: " << payload.raw_size << '\n';
  Something* s = new Something(nullptr);
  memcpy(s, payload.data, payload.raw_size);
  std::cout << "DeserializeInternalFields payload value: " << s->value() << '\n';
  holder->SetAlignedPointerInInternalField(index, s);
}

TEST_F(SnapshotTest, InternalFields) {
  /*
  const char* trace = "--trace_serializer";
  v8::internal::FlagList::SetFlagsFromString(trace, strlen(trace));
  */

  StartupData startup_data;
  size_t index = 0;


  // Serialize callback which also takes a pointer to the internal field
  // so that it can be written out.
  SerializeInternalFieldsCallback si_cb = SerializeInternalFieldsCallback(
      SerializeInternalFields, nullptr);

  int context_index;
  {
    Isolate* isolate = nullptr;
    isolate = Isolate::Allocate();
    SnapshotCreator snapshot_creator(isolate, nullptr);
    {
      HandleScope scope(isolate);
      snapshot_creator.SetDefaultContext(Context::New(isolate));
      Local<Context> context = Context::New(isolate);
      {
        Context::Scope context_scope(context);
        TryCatch try_catch(isolate);

        Local<Object> global = context->Global();

        // This is the data that we want to add as an internal field
        Something s("Some data...");

        Local<FunctionTemplate> constructor = Local<FunctionTemplate>();
        Local<ObjectTemplate> ot = ObjectTemplate::New(isolate, constructor);
        ot->SetInternalFieldCount(1);

        MaybeLocal<Object> maybe_instance = ot->NewInstance(context);
        Local<Object> obj = maybe_instance.ToLocalChecked();
        Local<String> obj_name = String::NewFromUtf8Literal(isolate, "something");
        obj->SetAlignedPointerInInternalField(0, static_cast<void*>(&s));
        global->Set(context, obj_name, obj).Check();
      }

      context_index = snapshot_creator.AddContext(context, si_cb);
    }
    startup_data = snapshot_creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kKeep);
  }

  Isolate::CreateParams create_params;
  create_params.snapshot_blob = &startup_data;
  create_params.array_buffer_allocator = ArrayBuffer::Allocator::NewDefaultAllocator();
  Isolate* isolate = Isolate::New(create_params);

  DeserializeInternalFieldsCallback di_cb = DeserializeInternalFieldsCallback(
      DeserializeInternalFields, nullptr);
  {
    HandleScope scope(isolate);
    Local<Context> context = Context::FromSnapshot(isolate, context_index, di_cb).ToLocalChecked();

    Local<String> obj_name = String::NewFromUtf8Literal(isolate, "something");
    Local<Object> global = context->Global();
    MaybeLocal<Value> maybe_obj = global->Get(context, obj_name);
    EXPECT_FALSE(maybe_obj.IsEmpty());

    Local<Object> obj = Local<Object>::Cast(maybe_obj.ToLocalChecked());
    Something* ptr = static_cast<Something*>(obj->GetAlignedPointerFromInternalField(0));
    std::cout << "Something was deserialized: " << ptr->value() << '\n';
    EXPECT_STREQ("Some data...", ptr->value());
  }
  isolate->Dispose();
}
