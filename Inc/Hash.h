
#pragma once

#include "Allocator.h"
#include <new>

template <typename T>
class CHash
{
public:
	struct Hash_t
	{
		const void* key;	// pointer that we have hashed in the hash table (the 'key' of the key/value pair)
		T* value;			// pointer to the record that we are hashing (the 'value' of the key/value pair)
		Hash_t* Next;		// pointer to the next Hash_t structure in the linked list for this slot (to handle hash collisions)
	};

	CAllocator* HashAllocator;

	volatile int HashTableSize;
	volatile Hash_t** HashTable;		// array of pointers to Hash_t structs

	volatile Hash_t** OldHashTable;		// this points to the next free byte from the old hash table (after calling IncreaseHashTableSize())
	size_t OldHashTableFreeRemaining;

	unsigned int NumUsedSlots;		// number of slots were are currently using in the hash table array
	unsigned int MaxListLength;		// the length of the longest linked list currently in the hash table
	unsigned int NumTotalRecords;	// the total number of hash records we have in this hash table

	CHash(CAllocator* InHashAllocator, int InHashTableSize, bool bInWaitOnMutex = false) :
		HashAllocator(InHashAllocator)
		,HashTableSize(InHashTableSize)
		,NumUsedSlots(0)
		,MaxListLength(0)
		,NumTotalRecords(0)
	{
		HashTable = nullptr;
		if( HashAllocator && HashTableSize )
		{
			HashTable = (volatile Hash_t**)HashAllocator->AllocateBytes(HashTableSize * sizeof(Hash_t*), sizeof(void*));
		}
	}

	~CHash() {}

	void PrintStats(char* Header, int NestLevel)
	{
		char buffer[64];
		buffer[0] = 0;

		for( int i = 0; (i < NestLevel) && (i < 32); i++ )
		{
			strcat_s(buffer, sizeof(buffer), "  ");
		}

		float probe_average = 0.f;
		if( NumTotalRecords > 0 )
		{
			probe_average = (float)NumTotalRecords / (float)NumUsedSlots;
		}

		DebugLog("%s%sCHash Stats: used = %d slots of total = %d, total records = %d, max list records = %d, probe average = %.3f", buffer, Header, NumUsedSlots, HashTableSize, NumTotalRecords, MaxListLength, probe_average);
		
		for( int i = 0; i < HashTableSize; i++ )
		{
			Hash_t* p = (Hash_t*)HashTable[i];
			while( p )
			{
				p->value->PrintStats("", NestLevel + 1);
				p = p->Next;
			}
		}
	}

	void** CopyHashToArray(CAllocator* InCopyAllocator, unsigned int& OutArraySize, bool bCopyMemberHashTables)  // copy this hash table to a fixed size array
	{
		OutArraySize = 0;

		void** pArrayOfPointers = nullptr;

		if( InCopyAllocator && HashTable && NumTotalRecords )
		{
			unsigned int NumTotalRecordsToCopy = 0;

			// get the actual number of records that need to be copied (this can be less than NumTotalRecords for calltree records that have been reset to zero)
			for( int i = 0; i < HashTableSize; i++ )
			{
				Hash_t* p = (Hash_t*)HashTable[i];
				while( p )
				{
					NumTotalRecordsToCopy += p->value->GetNumRecordsToCopy();

					p = p->Next;
				}
			}

			if( NumTotalRecordsToCopy )
			{
				pArrayOfPointers = (void**)InCopyAllocator->AllocateBytes(NumTotalRecordsToCopy * sizeof(void*), sizeof(void*));

				int ArrayIndex = 0;

				for( int i = 0; i < HashTableSize; i++ )
				{
					Hash_t* p = (Hash_t*)HashTable[i];
					while( p )
					{
						void* ptr = nullptr;

						if( p->value->GetNumRecordsToCopy() )
						{
							if( bCopyMemberHashTables )  // if doing a "deep" copy, then copy the hash table entry record and store the pointer to it
							{
								ptr = p->value->GetArrayCopy(InCopyAllocator, bCopyMemberHashTables);
							}
							else  // otherwise, just copy the hash table entry pointer (for parents and children) and we'll fix this up to point to the array copy of this record later
							{
								ptr = p->value;
							}

							pArrayOfPointers[ArrayIndex++] = ptr;
						}

						p = p->Next;
					}
				}
			}

			OutArraySize = NumTotalRecordsToCopy;
			return pArrayOfPointers;
		}

		return nullptr;
	}

	void ResetCounters(DWORD64 TimeNow)
	{
		if( HashTable && NumTotalRecords )
		{
			for( int i = 0; i < HashTableSize; i++ )
			{
				Hash_t* p = (Hash_t*)HashTable[i];
				while( p )
				{
					p->value->ResetCounters(TimeNow);
					p = p->Next;
				}
			}
		}
	}

	unsigned long HashPointer(const void* p)  // computes a hash value for a pointer value
	{
		unsigned long long a, b, c;

		// seed the initial values (with "magic" constant, see http://en.wikipedia.org/wiki/Tiny_Encryption_Algorithm and http://burtleburtle.net/bob/hash/doobs.html)
		a = b = 0x9e3779b9;
		c = 0;

		a += reinterpret_cast<unsigned long long>(p);

		// from https://gist.github.com/badboy/6267743
		a -= b; a -= c; a ^= (c>>13);
		b -= c; b -= a; b ^= (a<<8);
		c -= a; c -= b; c ^= (b>>13);
		a -= b; a -= c; a ^= (c>>12);
		b -= c; b -= a; b ^= (a<<16);
		c -= a; c -= b; c ^= (b>>5);
		a -= b; a -= c; a ^= (c>>3);
		b -= c; b -= a; b ^= (a<<10);
		c -= a; c -= b; c ^= (b>>15);

		return (long)c;
	}

	void* AllocateFromOldHashTable()  // allocates a Hash_t struct from the previously "freed" hash table (after IncreaseHashTableSize() was called)
	{
		// OldHashTable always points to the next aligned free byte from the old hash table
		char* FreePointer = (char*)OldHashTable;

		int Alignment = sizeof(void*);

		// point to the next free aligned Hash_t address...
		char* pAlignedFreePointer = (char*)(((uintptr_t)(FreePointer + sizeof(Hash_t) + Alignment - 1) / Alignment) * Alignment);
		size_t AlignedOffset = pAlignedFreePointer - FreePointer;
		OldHashTableFreeRemaining = OldHashTableFreeRemaining - AlignedOffset;

		if( OldHashTableFreeRemaining >= sizeof(Hash_t) )  // if there is room left for another Hash_t allocation...
		{
			OldHashTable = (volatile Hash_t**)pAlignedFreePointer;  // update the OldHashTable pointer to point to the next free aligned address
		}
		else  // otherwise, we're out of room so null out the OldHashTable pointer (so we don't use it anymore)
		{
			OldHashTable = nullptr;
			OldHashTableFreeRemaining = 0;
		}

		return (void*)FreePointer;
	}

	T** LookupPointer(const void* InPointer)  // for the given InPointer, return the address of the 'value' pointer in the Hash_t struct
	{
		// hash the input pointer and find the record, if it doesn't exist then add a new record to the hash table
		unsigned long hash = HashPointer(InPointer);

		hash = hash % HashTableSize;

		volatile Hash_t* pHashRecord = HashTable[hash];
		volatile Hash_t* pPrevHashRecord = nullptr;

		unsigned int LinkedListLength = 0;

		while( pHashRecord )
		{
			LinkedListLength++;
			if( LinkedListLength > MaxListLength )
			{
				MaxListLength = LinkedListLength;
			}

			if( pHashRecord->key == InPointer )
			{
				return &(T*)pHashRecord->value;
			}

			pPrevHashRecord = pHashRecord;  // save the previous pointer (to link in a new one below)
			pHashRecord = pHashRecord->Next;
		}

		// didn't find it, so add a new one to the hash table...

		Hash_t* pNewHashRec = nullptr;

		if( OldHashTable )  // if we have an old hash table (after re-sizing), use that space to allocate a new Hash_t record
		{
			pNewHashRec = (Hash_t*)AllocateFromOldHashTable();
		}
		else
		{
			pNewHashRec = (Hash_t*)HashAllocator->AllocateBytes(sizeof(Hash_t), sizeof(void*));
		}

		pNewHashRec->key = InPointer;  // store the 'key'

		// NOTE: This 'value' pointer is required to be initialized by whatever code called the LookupPointer() function.
		// It needs to check what the returned pointer points to and if it's null, allocate the T record and assign that
		// to the hash tables 'value' pointer.  This will become the pointer to 'value' returned on subsequent lookups.

		pNewHashRec->value = nullptr;
		pNewHashRec->Next = nullptr;

		// if there's no previous pointer just add this new one to the hash table (i.e. this slot is empty)
		if( pPrevHashRecord == nullptr )
		{
			HashTable[hash] = pNewHashRec;
			NumUsedSlots++;
		}
		else  // otherwise, add this new record to the end of the linked list
		{
			pPrevHashRecord->Next = pNewHashRec;
		}

		NumTotalRecords++;

		float probe_average = (float)NumTotalRecords / (float)NumUsedSlots;

		// see if we need to expand the hash table size
		if( (NumUsedSlots > (unsigned int)((float)HashTableSize * 0.8f)) ||  // have we used more than 80% of the slots?
			(probe_average > 5.0f) ||  // on average, does it take more than 5 probes through the linked list to find a match?
			(MaxListLength > 10) )  // is the maximum length of any linked list in the hash table more than 10 nodes?
		{
			IncreaseHashTableSize();

			// find it again now that the hash table has changed
			unsigned long rehash = HashPointer(InPointer);

			rehash = rehash % HashTableSize;

			volatile Hash_t* pHashRecord = HashTable[rehash];

			while( pHashRecord )
			{
				if( pHashRecord->key == InPointer )
				{
					return &(T*)pHashRecord->value;
				}

				pHashRecord = pHashRecord->Next;
			}

			DebugLog("CHash.LookupPointer(): Failed to find record after IncreaseHashTableSize()!");
			assert(false);
		}

		return &pNewHashRec->value;
	}

	void IncreaseHashTableSize()  // increase the size of this hash table to reduce collisions
	{
		volatile int OldHashTableSize = HashTableSize;
		OldHashTableFreeRemaining = OldHashTableSize * sizeof(Hash_t*);
		OldHashTable = HashTable;

		HashTableSize = HashTableSize * 2;
		HashTable = (volatile Hash_t**)HashAllocator->AllocateBytes(HashTableSize * sizeof(Hash_t*), sizeof(void*));

		NumUsedSlots = 0;
		MaxListLength = 0;
		NumTotalRecords = 0;

		for( int i = 0; i < OldHashTableSize; i++ )
		{
			Hash_t* p = (Hash_t*)OldHashTable[i];
			while( p )
			{
				unsigned long new_rehash = HashPointer(p->key);

				new_rehash = new_rehash % HashTableSize;

				volatile Hash_t* pHashRecord = HashTable[new_rehash];
				volatile Hash_t* pPrevHashRecord = nullptr;

				unsigned int LinkedListLength = 0;

				while( pHashRecord )
				{
					LinkedListLength++;
					if( LinkedListLength > MaxListLength )
					{
						MaxListLength = LinkedListLength;
					}

					pPrevHashRecord = pHashRecord;  // save the previous pointer (to link in a new one below)
					pHashRecord = pHashRecord->Next;
				}

				pHashRecord = p;
				p = p->Next;  // bump to next pointer from old hash table before we clobber the record it points to

				pHashRecord->Next = nullptr;

				// if there's no previous pointer just add this new one to the hash table (i.e. this slot is empty)
				if( pPrevHashRecord == nullptr )
				{
					HashTable[new_rehash] = pHashRecord;
					NumUsedSlots++;
				}
				else  // otherwise, add this new record to the end of the linked list
				{
					pPrevHashRecord->Next = (Hash_t*)pHashRecord;
				}

				NumTotalRecords++;
			}
		}
	}

private:
	CHash(const CHash& other, CAllocator* InHashAllocator = nullptr) :  // copy constructor (this should never get called)
		HashAllocator(InHashAllocator)
	{
		assert(false);
	}

	CHash& operator=(const CHash&)  // assignment operator (this should never get called)
	{
		assert(false);
	}
};
