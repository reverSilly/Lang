//
// Created by Louis Li on 2023/8/11.
//

#ifndef COMPILER_TRIE_H
#define COMPILER_TRIE_H

#include<optional>
#include<unordered_map>
#include<cstdint>
#include<span>

namespace reverSilly
{
	template<typename V>
	class Trie
	{
		struct node
		{
			std::unordered_map<uint8_t,node>sons;
			std::optional<V> value;
		};
		node rt;
		void insert(std::span<uint8_t>key,const V &v)
		{
			//TODO
		}
		V& operator[]()
		{
			//TODO
		}
	};
} // reverSilly

#endif //COMPILER_TRIE_H
