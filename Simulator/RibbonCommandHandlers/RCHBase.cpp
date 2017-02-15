
#include "pch.h"
#include "RCHBase.h"
#include "../Win32Defs.h"

using namespace std;

// C++03 standard 3.6.2 assures us that the runtime initializes these POD types
// to zero before it constructs any static instance of TypeInfo.
static unordered_set<const RCHInfo*>* chInfos = nullptr;

const std::unordered_set<const RCHInfo*>& GetRCHInfos()
{
	assert (chInfos != nullptr);
	return *chInfos;
}

RCHInfo::RCHInfo (std::unordered_set<UINT32>&& commands, RCHFactory factory)
	: _commands(move(commands)), _factory(factory)
{
	if (chInfos == nullptr)
		chInfos = new unordered_set<const RCHInfo*>();
	chInfos->insert(this);
}

RCHInfo::~RCHInfo()
{
	chInfos->erase(this);
	if (chInfos->empty())
	{
		delete chInfos;
		chInfos = nullptr;
	}
}

RCHBase::RCHBase (IProjectWindow* pw, IUIFramework* rf, IProject* project, ISelection* selection)
	: _pw(pw), _rf(rf), _project(project), _selection(selection)
{
	_selection->GetSelectionChangedEvent().AddHandler(&OnSelectionChangedStatic, this);
}

RCHBase::~RCHBase()
{
	_selection->GetSelectionChangedEvent().RemoveHandler(&OnSelectionChangedStatic, this);
}

//static
void RCHBase::OnSelectionChangedStatic(void* callbackArg, ISelection* selection)
{
	auto rch = static_cast<RCHBase*>(callbackArg);
	rch->OnSelectionChanged();
}

HRESULT RCHBase::QueryInterface(REFIID riid, void **ppvObject)
{
	if (!ppvObject)
		return E_INVALIDARG;

	*ppvObject = NULL;
	if (riid == __uuidof(IUnknown))
	{
		*ppvObject = static_cast<IUnknown*>((IProjectWindow*) this);
		AddRef();
		return S_OK;
	}
	else if (riid == __uuidof(IUICommandHandler))
	{
		*ppvObject = static_cast<IUICommandHandler*>(this);
		AddRef();
		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG RCHBase::AddRef()
{
	return InterlockedIncrement (&_refCount);
}

ULONG RCHBase::Release()
{
	ULONG newRefCount = InterlockedDecrement(&_refCount);
	if (newRefCount == 0)
		delete this;
	return newRefCount;
}
