#include "Core/Core.h"
#include "Core/Platform.h"
#include "Runtime.h"
#include "RuntimePrivate.h"

namespace Runtime
{
	void init()
	{
		LLVMJIT::init();
		initWAVMIntrinsics();
	}
	
	// Returns a vector of strings, each element describing a frame of the call stack.
	// If the frame is a JITed function, use the JIT's information about the function
	// to describe it, otherwise fallback to whatever platform-specific symbol resolution
	// is available.
	std::vector<std::string> describeCallStack(const Platform::CallStack& callStack)
	{
		std::vector<std::string> frameDescriptions;
		for(auto frame : callStack.stackFrames)
		{
			std::string frameDescription;
			if(	LLVMJIT::describeInstructionPointer(frame.ip,frameDescription)
			||	Platform::describeInstructionPointer(frame.ip,frameDescription))
			{
				frameDescriptions.push_back(frameDescription);
			}
			else { frameDescriptions.push_back("<unknown function>"); }
		}
		return frameDescriptions;
	}

	[[noreturn]] void causeException(Exception::Cause cause)
	{
		auto callStack = Platform::captureCallStack();
		throw Exception {cause,describeCallStack(callStack)};
	}

	bool isA(Object* object,const ObjectType& type)
	{
		if(type.kind != object->kind) { return false; }

		switch(type.kind)
		{
		case ObjectKind::function: return type.function == asFunction(object)->type;
		case ObjectKind::global: return type.global == asGlobal(object)->type;
		case ObjectKind::table:
		{
			auto table = asTable(object);
			return type.table.elementType == table->type.elementType
				&&	isSubset(type.table.size,table->type.size);
		}
		case ObjectKind::memory:
		{
			auto memory = asMemory(object);
			return isSubset(type.memory.size,memory->type.size);
		}
		default: Core::unreachable();
		}
	}

	Result invokeFunction(FunctionInstance* function,const std::vector<Value>& parameters)
	{
		const FunctionType* functionType = function->type;
		
		// Check that the parameter types match the function, and copy them into a memory block that stores each as a 64-bit value.
		if(parameters.size() != functionType->parameters.size())
		{ throw Exception {Exception::Cause::invokeSignatureMismatch}; }

		uint64* thunkMemory = (uint64*)alloca((functionType->parameters.size() + getArity(functionType->ret)) * sizeof(uint64));
		for(uintp parameterIndex = 0;parameterIndex < functionType->parameters.size();++parameterIndex)
		{
			if(functionType->parameters[parameterIndex] != parameters[parameterIndex].type)
			{
				throw Exception {Exception::Cause::invokeSignatureMismatch};
			}

			thunkMemory[parameterIndex] = parameters[parameterIndex].i64;
		}
		
		// Get the invoke thunk for this function type.
		LLVMJIT::InvokeFunctionPointer invokeFunctionPointer = LLVMJIT::getInvokeThunk(functionType);

		// Catch platform-specific runtime exceptions and turn them into Runtime::Values.
		Result result;
		Platform::HardwareTrapType trapType;
		Platform::CallStack trapCallStack;
		Platform::CallStack callerStack;
		uintp trapOperand;
		trapType = Platform::catchHardwareTraps(trapCallStack,trapOperand,
			[&]
			{
				callerStack = Platform::captureCallStack();

				// Call the invoke thunk.
				(*invokeFunctionPointer)(function->nativeFunction,thunkMemory);

				// Read the return value out of the thunk memory block.
				if(functionType->ret != ResultType::none)
				{
					result.type = functionType->ret;
					result.i64 = thunkMemory[functionType->parameters.size()];
				}
			});

		// If there was no hardware trap, just return the result.
		if(trapType == Platform::HardwareTrapType::none) { return result; }
		else
		{		
			// Truncate the stack frame to the native code invoking the function.
			if(trapCallStack.stackFrames.size() >= callerStack.stackFrames.size() + 1)
			{
				trapCallStack.stackFrames.resize(trapCallStack.stackFrames.size() - callerStack.stackFrames.size() -1);
			}

			std::vector<std::string> callStackDescription = describeCallStack(trapCallStack);

			switch(trapType)
			{
			case Platform::HardwareTrapType::accessViolation:
			{
				// If the access violation occured in a Table's reserved pages, treat it as an undefined table element runtime error.
				if(isAddressOwnedByTable(reinterpret_cast<uint8*>(trapOperand))) { throw Exception { Exception::Cause::undefinedTableElement, callStackDescription }; }
				// If the access violation occured in a Memory's reserved pages, treat it as an access violation runtime error.
				else if(isAddressOwnedByMemory(reinterpret_cast<uint8*>(trapOperand))) { throw Exception { Exception::Cause::accessViolation, callStackDescription }; }
				else
				{
					// If the access violation occured outside of a Table or Memory, treat it as a bug (possibly a security hole)
					// rather than a runtime error in the WebAssembly code.
					Log::printf(Log::Category::error,"Access violation outside of table or memory reserved addresses. Call stack:\n");
					for(auto calledFunction : callStackDescription) { Log::printf(Log::Category::error,"  %s\n",calledFunction.c_str()); }
					Core::errorf("");
				}
			}
			case Platform::HardwareTrapType::stackOverflow: throw Exception { Exception::Cause::stackOverflow, callStackDescription };
			case Platform::HardwareTrapType::intDivideByZeroOrOverflow: throw Exception { Exception::Cause::integerDivideByZeroOrIntegerOverflow, callStackDescription };
			default: Core::unreachable();
			};
		}
	}

	const FunctionType* getFunctionType(FunctionInstance* function)
	{
		return function->type;
	}

	GlobalInstance* createGlobal(GlobalType type,Value initialValue)
	{
		return new GlobalInstance(type,initialValue);
	}

	Value getGlobalValue(GlobalInstance* global)
	{
		return Value(global->type.valueType,global->value);
	}

	Value setGlobalValue(GlobalInstance* global,Value newValue)
	{
		assert(newValue.type == global->type.valueType);
		assert(global->type.isMutable);
		const Value previousValue = Value(global->type.valueType,global->value);
		global->value = newValue;
		return previousValue;
	}
}