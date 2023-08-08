#include "EtwLogger.h"

#include "vector.h"

#define va_copy(dest, src) (dest = src)

// Cache of all created providers.
MyVector<detail::EtwProvider> g_ProviderCache;

typedef enum _ETW_FIELD_TYPE
{
	EtwFieldNull,
	EtwFieldUnicodeString,
	EtwFieldAnsiString,
	EtwFieldInt8,
	EtwFieldUInt8,
	EtwFieldInt16,
	EtwFieldUInt16,
	EtwFieldInt32,
	EtwFieldUInt32,
	EtwFieldInt64,
	EtwFieldUInt64,
	EtwFieldFloat,
	EtwFieldDouble,
	EtwFieldBool32,
	EtwFieldBinary,
	EtwFieldGuid,
	EtwFieldPointer,
	EtwFieldFiletime,
	EtwFieldSystemTime,
	EtwFieldSid,
	EtwFieldHexInt32,
	EtwFieldHexInt64,
	EtwFieldPid = (EtwFieldInt32 | 0x05 << 8),
} ETW_FIELD_TYPE;

namespace detail
{

#pragma region EtwProviderEvent

EtwProviderEvent::EtwProviderEvent() : m_eventMetadataDesc{}
{
}

NTSTATUS EtwProviderEvent::Initialize(const char* eventName, int numberOfFields, va_list fields)
{
	// Allocate the total size of the event metadata structure.
	//
	// This consists of, in order:
	//
	// * The total length of the structure
	// * The event tag byte (currently always 0)
	// * The name of the event
	// * An array of field metadata structures, which are:
	//     * the name of the field
	//     * a single byte for the type.
	const auto eventMetadataHeaderLength = strlen(eventName) + 1 + sizeof(uint16_t) + sizeof(uint8_t);

	// Calculate the total size to allocate for the event metadata - the size
	// of the header plus the sum of length of each field name and the type byte.
	va_list args;
	va_copy(args, fields);
	auto eventMetadataLength = (uint16_t)eventMetadataHeaderLength;
	for (auto i = 0; i < numberOfFields; i++)
	{
		const auto fieldName = va_arg(args, const char*);
		eventMetadataLength += (uint16_t)(strlen(fieldName) + 1 + sizeof(uint8_t));
		va_arg(args, int);
		va_arg(args, void*);
	}
	va_end(args);

	const auto eventMetadata = (struct EventMetadata*)ExAllocatePoolWithTag(NonPagedPoolNx, eventMetadataLength, 'wteE');
	if (eventMetadata == NULL)
	{
		return STATUS_UNSUCCESSFUL;
	}

	memset(eventMetadata, 0, eventMetadataLength);

	// Set the first three fields, metadata about the event.
	eventMetadata->TotalLength = eventMetadataLength;
	eventMetadata->Tag = 0;
	strcpy(eventMetadata->EventName, eventName);

	// Set the metadata for each field.
	char* currentLocation = ((char*)eventMetadata) + eventMetadataHeaderLength;
	va_copy(args, fields);
	for (auto i = 0; i < numberOfFields; i++)
	{
		const auto fieldName = va_arg(args, const char*);
		const auto fieldType = va_arg(args, int);
		va_arg(args, void*);

		strcpy(currentLocation, fieldName);
		currentLocation[strlen(fieldName) + 1] = (uint8_t)fieldType;

		currentLocation += strlen(fieldName) + 1 + sizeof(uint8_t);
	}
	va_end(args);

	// Create an EVENT_DATA_DESCRIPTOR pointing to the metadata.
	EventDataDescCreate(&m_eventMetadataDesc, eventMetadata, eventMetadata->TotalLength);
	m_eventMetadataDesc.Type = EVENT_DATA_DESCRIPTOR_TYPE_EVENT_METADATA;  // Descriptor contains event metadata.

	return STATUS_SUCCESS;
}

void EtwProviderEvent::Destruct()
{
	if (m_eventMetadataDesc.Ptr != NULL)
	{
		ExFreePool((PVOID)m_eventMetadataDesc.Ptr);
	}
}

const char* EtwProviderEvent::Name() const
{
	return ((EventMetadata*)m_eventMetadataDesc.Ptr)->EventName;
}

EVENT_DATA_DESCRIPTOR EtwProviderEvent::MetadataDescriptor() const
{
	return m_eventMetadataDesc;
}

#pragma endregion

#pragma region EtwProvider

EtwProvider::EtwProvider(LPCGUID providerGuid) : m_guid{ providerGuid }, m_regHandle{}, m_providerMetadataDesc{}, m_events{}
{
}

EtwProvider::EtwProvider(EtwProvider&& other)
{
	m_guid = move(other.m_guid);
	m_regHandle = move(other.m_regHandle);
	m_providerMetadataDesc = move(other.m_providerMetadataDesc);
	m_events = move(other.m_events);
}

EtwProvider& EtwProvider::operator=(EtwProvider&& other)
{
	m_guid = move(other.m_guid);
	m_regHandle = move(other.m_regHandle);
	m_providerMetadataDesc = move(other.m_providerMetadataDesc);
	m_events = move(other.m_events);

	return *this;
}

NTSTATUS EtwProvider::Initialize(const char* providerName)
{
	// Register the kernel-mode ETW provider.
	auto status = EtwRegister(m_guid, NULL, NULL, &m_regHandle);
	if (status != STATUS_SUCCESS)
	{
		return status;
	}

	// Create packaged provider metadata structure.
	// <https://learn.microsoft.com/en-us/windows/win32/etw/provider-traits>
	const auto providerMetadataLength = (uint16_t)((strlen(providerName) + 1) + sizeof(uint16_t));
	const auto providerMetadata = (struct ProviderMetadata*)ExAllocatePoolWithTag(NonPagedPoolNx, providerMetadataLength, 'wteO');
	if (providerMetadata == NULL)
	{
		return STATUS_UNSUCCESSFUL;
	}

	memset(providerMetadata, 0, providerMetadataLength);
	providerMetadata->TotalLength = providerMetadataLength;
	strcpy(providerMetadata->ProviderName, providerName);

	// Tell the provider to use the metadata structure.
	status = EtwSetInformation(m_regHandle, EventProviderSetTraits, providerMetadata, providerMetadataLength);
	if (status != STATUS_SUCCESS)
	{
		return status;
	}

	// Create an EVENT_DATA_DESCRIPTOR pointing to the metadata.
	EventDataDescCreate(&m_providerMetadataDesc, providerMetadata, providerMetadata->TotalLength);
	m_providerMetadataDesc.Type = EVENT_DATA_DESCRIPTOR_TYPE_PROVIDER_METADATA;  // Descriptor contains provider metadata.

	return STATUS_SUCCESS;
}

void EtwProvider::Destruct()
{
	for (auto i = 0; i < m_events.len(); i++)
	{
		m_events[i].Destruct();
	}
	m_events.Destruct();

	if (m_regHandle != 0)
	{
		EtwUnregister(m_regHandle);
	}

	if (m_providerMetadataDesc.Ptr != NULL)
	{
		ExFreePool((PVOID)m_providerMetadataDesc.Ptr);
	}
}

NTSTATUS EtwProvider::AddEvent(const char* eventName, int numberOfFields, va_list fields)
{
	auto status = STATUS_SUCCESS;

	if (FindEvent(eventName) == NULL)
	{
		EtwProviderEvent event;
		status = event.Initialize(eventName, numberOfFields, fields);
		if (status != STATUS_SUCCESS)
		{
			return status;
		}
		m_events.push_back(move(event));
	}

	return status;
}

NTSTATUS EtwProvider::WriteEvent(const char* eventName, PCEVENT_DESCRIPTOR eventDescriptor, int numberOfFields, va_list fields)
{
	// Find the event to use.
	const auto event = FindEvent(eventName);
	if (event == NULL)
	{
		return STATUS_UNSUCCESSFUL;
	}

	// Allocate space for the data descriptors, including two additional slots
	// for provider and event metadata.
	const auto numberOfDescriptors = numberOfFields + 2;
	const auto allocSize = numberOfDescriptors * sizeof(EVENT_DATA_DESCRIPTOR);
	const auto dataDescriptors = (PEVENT_DATA_DESCRIPTOR)ExAllocatePoolWithTag(NonPagedPoolNx, allocSize, 'wteP');
	if (dataDescriptors == NULL)
	{
		return STATUS_UNSUCCESSFUL;
	}
	memset(dataDescriptors, 0, allocSize);

	// Set the provider and event metadata descriptors.
	dataDescriptors[0] = m_providerMetadataDesc;
	dataDescriptors[1] = event->MetadataDescriptor();

	// Create a descriptor for each individual field.
	va_list args;
	va_copy(args, fields);
	for (auto i = 0; i < numberOfFields; i++)
	{
		va_arg(args, const char*);
		const auto fieldType = va_arg(args, int);
		auto fieldValue = va_arg(args, size_t);

		dataDescriptors[i + 2] = CreateTraceProperty(
			fieldType,
			fieldType != EtwFieldAnsiString ? &fieldValue : (void*)fieldValue);
		if (dataDescriptors[i + 2].Ptr == NULL)
		{
			return STATUS_UNSUCCESSFUL;
		}
	}
	va_end(args);

	// Write the event.
	return EtwWrite(m_regHandle, eventDescriptor, NULL, numberOfDescriptors, dataDescriptors);

	// TODO: This leaks the memory allocated for the field descriptors
}

LPCGUID EtwProvider::Guid() const noexcept
{
	return m_guid;
}

EtwProviderEvent* EtwProvider::FindEvent(const char* eventName)
{
	for (auto i = 0; i < m_events.len(); i++)
	{
		if (strcmp(eventName, m_events[i].Name()) == 0)
		{
			return &m_events[i];
		}
	}

	return NULL;
}

__declspec(noinline) size_t EtwProvider::SizeOfField(int fieldType, void* fieldValue)
{
	size_t sizeOfField = 0;

	switch (fieldType & 0x000000FF)
	{
	case EtwFieldAnsiString:
		sizeOfField = strlen((char*)fieldValue) + 1;
		break;
	case EtwFieldInt8:
		sizeOfField = sizeof(int8_t);
		break;
	case EtwFieldUInt8:
		sizeOfField = sizeof(uint8_t);
		break;
	case EtwFieldInt16:
		sizeOfField = sizeof(int16_t);
		break;
	case EtwFieldUInt16:
		sizeOfField = sizeof(uint16_t);
		break;
	case EtwFieldInt32:
		sizeOfField = sizeof(int32_t);
		break;
	case EtwFieldUInt32:
		sizeOfField = sizeof(uint32_t);
		break;
	case EtwFieldInt64:
		sizeOfField = sizeof(int64_t);
		break;
	case EtwFieldUInt64:
		sizeOfField = sizeof(uint64_t);
		break;
	case EtwFieldFloat:
		sizeOfField = sizeof(float);
		break;
	case EtwFieldDouble:
		sizeOfField = sizeof(double);
		break;
	case EtwFieldBool32:
		sizeOfField = 4;
		break;
	case EtwFieldGuid:
		sizeOfField = sizeof(GUID);
		break;
		// TODO: more fields
	default:
		sizeOfField = 0;
		break;
	}

	return sizeOfField;
}

__declspec(noinline) EVENT_DATA_DESCRIPTOR EtwProvider::CreateTraceProperty(int fieldType, void* fieldValue)
{
	// Copy the input value to its own space.
	EVENT_DATA_DESCRIPTOR fieldDesc;
	memset(&fieldDesc, 0, sizeof(EVENT_DATA_DESCRIPTOR));

	const auto fieldSize = SizeOfField(fieldType, fieldValue);
	const auto newSpace = ExAllocatePoolWithTag(NonPagedPoolNx, fieldSize, 'wteE');
	if (newSpace == NULL)
	{
		return fieldDesc;
	}
	memcpy(newSpace, fieldValue, fieldSize);

	// Create the event data descriptor pointing to the value of the field.
	EventDataDescCreate(&fieldDesc, newSpace, (ULONG)fieldSize);

	return fieldDesc;
}

#pragma endregion

} // namespace detail

detail::EtwProvider* FindProvider(LPCGUID providerGuid)
{
	for (auto i = 0; i < g_ProviderCache.len(); i++)
	{
		if ((g_ProviderCache[i].Guid()->Data1 == providerGuid->Data1) &&
			(g_ProviderCache[i].Guid()->Data2 == providerGuid->Data2) &&
			(g_ProviderCache[i].Guid()->Data3 == providerGuid->Data3) &&
			(g_ProviderCache[i].Guid()->Data4[0] == providerGuid->Data4[0]) &&
			(g_ProviderCache[i].Guid()->Data4[1] == providerGuid->Data4[1]) &&
			(g_ProviderCache[i].Guid()->Data4[2] == providerGuid->Data4[2]) &&
			(g_ProviderCache[i].Guid()->Data4[3] == providerGuid->Data4[3]) &&
			(g_ProviderCache[i].Guid()->Data4[4] == providerGuid->Data4[4]) &&
			(g_ProviderCache[i].Guid()->Data4[5] == providerGuid->Data4[5]) &&
			(g_ProviderCache[i].Guid()->Data4[6] == providerGuid->Data4[6]) &&
			(g_ProviderCache[i].Guid()->Data4[7] == providerGuid->Data4[7]))
		{
			return &g_ProviderCache[i];
		}
	}

	return NULL;
}

__declspec(noinline) EVENT_DESCRIPTOR CreateEventDescriptor(uint64_t keyword, uint8_t level)
{
	EVENT_DESCRIPTOR desc;
	memset(&desc, 0, sizeof(EVENT_DESCRIPTOR));
	desc.Channel = 11;  // All "manifest-free" events should go to channel 11 by default
	desc.Keyword = keyword;
	desc.Level = level;

	return desc;
}

NTSTATUS EtwTrace(
	const char* providerName,
	const GUID* providerGuid,
	const char* eventName,
	uint8_t eventLevel,
	uint64_t keyword,
	int numberOfFields,
	...
)
{
	// It is unsafe to call EtwRegister() at higher than PASSIVE_LEVEL
	if (KeGetCurrentIrql() > PASSIVE_LEVEL)
	{
		return STATUS_UNSUCCESSFUL;
	}

	// If we have already made a provider with the given GUID, get it out of the
	// cache, otherwise create and register a new provider with the name and GUID.
	auto etwProvider = FindProvider(providerGuid);
	if (etwProvider == NULL)
	{
		detail::EtwProvider newEtwProvider{ providerGuid };
		const auto status = newEtwProvider.Initialize(providerName);
		if (status != STATUS_SUCCESS)
		{
			return status;
		}

		g_ProviderCache.push_back(move(newEtwProvider));
		etwProvider = &g_ProviderCache.back();
	}

	// Add the event to the provider.
	va_list args;
	va_start(args, numberOfFields);
	auto status = etwProvider->AddEvent(eventName, numberOfFields, args);
	if (status != STATUS_SUCCESS)
	{
		return status;
	}
	va_end(args);

	// Create the top-level event descriptor.
	const auto eventDesc = CreateEventDescriptor(keyword, eventLevel);

	// Write the event.
	va_list args2;
	va_start(args2, numberOfFields);
	status = etwProvider->WriteEvent(eventName, &eventDesc, numberOfFields, args2);
	va_end(args2);

	return status;
}