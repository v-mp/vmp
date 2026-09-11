#include "StdInc.h"

#include <jitasm.h>
#include "Hooking.Patterns.h"

#include "CrossBuildRuntime.h"

static HookFunction hookFunction([]()
{
	// CWeaponDamageEvent::Decide -> CWeaponDamageEvent::_Validate
	//
	// Near the end of the function, when handling damage type 2, the code reads
	// through `*parentEntity` (from CWeaponDamageEvent::parentGlobalId) without checking whether it is null first:
	auto location = hook::get_pattern<char>("F6 C1 ? 0F 85 ? ? ? ? ? ? ? ? ? ? FF 90 ? ? ? ? 48 85 C0", 9);

	// The relative offset to the reject path (mov al, 1) is stored in the `jnz` instruction right before `location`.
	auto rejectRel32 = *(int32_t*)(location - 4);
	auto rejectLocation = (uintptr_t)location + rejectRel32;

	// The vtable offset for the GetEntity() call, taken from the `call [rax+<getEntityVtOffset>]` instruction
	auto getEntityVtOffset = *(int32_t*)(location + 8);

	static struct : jitasm::Frontend
	{
		uintptr_t successLocation;
		uintptr_t rejectLocation;
		int32_t getEntityOffset;

		void Init(uintptr_t success, uintptr_t reject, int32_t entityOffset)
		{
			successLocation = success;
			rejectLocation = reject;
			getEntityOffset = entityOffset;
		}

		virtual void InternalMain() override
		{
			// rcx = *parentEntity
			mov(rcx, qword_ptr[r15]);

			test(rcx, rcx);
			jz("nullParent");

			// Original code: mov rax, [rcx]; call qword ptr [rax+disp32]
			mov(rax, qword_ptr[rcx]);
			mov(rax, qword_ptr[rax + getEntityOffset]);
			call(rax);

			mov(r11, successLocation);
			jmp(r11);

			L("nullParent");

			mov(r11, rejectLocation);
			jmp(r11);
		}
	} stub;

	stub.Init((uintptr_t)location + 12, rejectLocation, getEntityVtOffset);

	hook::nop(location, 12);
	hook::jump(location, stub.GetCode());
});
