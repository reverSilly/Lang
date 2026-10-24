//
// Created by Louis Li on 2023/8/11.
//

#ifndef COMPILER_OBJECT_H
#define COMPILER_OBJECT_H

#include "Trie.h"

#include<optional>
#include<vector>
#include<variant>
#include<cstdint>

namespace reverSilly
{
	namespace Lang
	{
		namespace Interpreter
		{
			class Type
			{
				struct IntegerInfo
				{
					bool signedness;
					uint32_t bits;
				};
				struct FloatInfo
				{
					uint32_t bits;
				};
				struct Member
				{
					uint32_t offset;
					std::optional<uint32_t>type; //index of type of the member, for type of member that has static type
				};
				uint32_t size;
				Trie<Member>members;
				std::variant<std::monostate,IntegerInfo,FloatInfo>builtins;
			};

			class Object
			{
				uint32_t type_index;
				std::vector<uint8_t>datum;
			};

		} // reverSilly
	} // Lang
} // Interpreter

#endif //COMPILER_OBJECT_H
