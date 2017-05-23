
#include "pch.h"
#include "Wire.h"
#include "Bridge.h"
#include "Port.h"

using namespace std;

static constexpr float WireThickness = 2;

Wire::Wire (WireEnd firstEnd, WireEnd secondEnd)
	: _points({ firstEnd, secondEnd })
{ }

// Workaround for what seems like a library bug: std::variant's operators == and != not working.
static bool Same (const WireEnd& a, const WireEnd& b)
{
	if (a.index() != b.index())
		return false;

	if (holds_alternative<LooseWireEnd>(a))
		return get<LooseWireEnd>(a) == get<LooseWireEnd>(b);
	else
		return get<ConnectedWireEnd>(a) == get<ConnectedWireEnd>(b);
}

void Wire::SetPoint (size_t pointIndex, const WireEnd& point)
{
	if (!Same(_points[pointIndex], point))
	{
		_points[pointIndex] = point;
		InvalidateEvent::InvokeHandlers(*this, this);
	}
}

D2D1_POINT_2F Wire::GetPointCoords (size_t pointIndex) const
{
	if (holds_alternative<LooseWireEnd>(_points[pointIndex]))
		return get<LooseWireEnd>(_points[pointIndex]);
	else
		return get<ConnectedWireEnd>(_points[pointIndex])->GetCPLocation();
}

bool Wire::IsForwarding (unsigned int vlanNumber, _Out_opt_ bool* hasLoop) const
{
	if (!holds_alternative<ConnectedWireEnd>(_points[0]) || !holds_alternative<ConnectedWireEnd>(_points[1]))
		return false;

	auto portA = get<ConnectedWireEnd>(_points[0]);
	auto portB = get<ConnectedWireEnd>(_points[1]);
	bool portAFw = portA->IsForwarding(vlanNumber);
	bool portBFw = portB->IsForwarding(vlanNumber);
	if (!portAFw || !portBFw)
		return false;

	if (hasLoop != nullptr)
	{
		unordered_set<Port*> txPorts;

		function<bool(Port* txPort)> transmitsTo = [vlanNumber, &txPorts, &transmitsTo, targetPort=portA](Port* txPort) -> bool
		{
			if (txPort->IsForwarding(vlanNumber))
			{
				auto rx = txPort->GetBridge()->GetProject()->FindConnectedPort(txPort);
				if ((rx != nullptr) && rx->IsForwarding(vlanNumber))
				{
					txPorts.insert(txPort);

					for (unsigned int i = 0; i < (unsigned int) rx->GetBridge()->GetPorts().size(); i++)
					{
						if ((i != rx->GetPortIndex()) && rx->IsForwarding(vlanNumber))
						{
							Port* otherTxPort = rx->GetBridge()->GetPorts()[i].get();
							if (otherTxPort == targetPort)
								return true;

							if (txPorts.find(otherTxPort) != txPorts.end())
								return false;

							if (transmitsTo(otherTxPort))
								return true;
						}
					}
				}
			}

			return false;
		};

		*hasLoop = transmitsTo(portA);
	}

	return true;
}

void Wire::Render (ID2D1RenderTarget* rt, const DrawingObjects& dos, unsigned int vlanNumber) const
{
	bool hasLoop;
	bool forwarding = IsForwarding (vlanNumber, &hasLoop);

	float width = WireThickness;
	ID2D1Brush* brush;

	if (!forwarding)
		brush = dos._brushNoForwardingWire;
	else if (!hasLoop)
		brush = dos._brushForwarding;
	else
	{
		brush = dos._brushLoop;
		width *= 2;
	}

	auto& ss = forwarding ? dos._strokeStyleForwardingWire : dos._strokeStyleNoForwardingWire;
	rt->DrawLine (GetP0Coords(), GetP1Coords(), brush, width, ss);
}

void Wire::RenderSelection (const IZoomable* zoomable, ID2D1RenderTarget* rt, const DrawingObjects& dos) const
{
	auto fd = zoomable->GetDLocationFromWLocation(GetP0Coords());
	auto td = zoomable->GetDLocationFromWLocation(GetP1Coords());

	float halfw = 10;
	float angle = atan2(td.y - fd.y, td.x - fd.x);
	float s = sin(angle);
	float c = cos(angle);

	array<D2D1_POINT_2F, 4> vertices =
	{
		D2D1_POINT_2F { fd.x + s * halfw, fd.y - c * halfw },
		D2D1_POINT_2F { fd.x - s * halfw, fd.y + c * halfw },
		D2D1_POINT_2F { td.x - s * halfw, td.y + c * halfw },
		D2D1_POINT_2F { td.x + s * halfw, td.y - c * halfw }
	};

	rt->DrawLine (vertices[0], vertices[1], dos._brushHighlight, 2, dos._strokeStyleSelectionRect);
	rt->DrawLine (vertices[1], vertices[2], dos._brushHighlight, 2, dos._strokeStyleSelectionRect);
	rt->DrawLine (vertices[2], vertices[3], dos._brushHighlight, 2, dos._strokeStyleSelectionRect);
	rt->DrawLine (vertices[3], vertices[0], dos._brushHighlight, 2, dos._strokeStyleSelectionRect);
}

HTResult Wire::HitTest (const IZoomable* zoomable, D2D1_POINT_2F dLocation, float tolerance)
{
	for (size_t i = 0; i < _points.size(); i++)
	{
		auto pointWLocation = this->GetPointCoords(i);
		auto pointDLocation = zoomable->GetDLocationFromWLocation(pointWLocation);
		D2D1_RECT_F rect = { pointDLocation.x, pointDLocation.y, pointDLocation.x, pointDLocation.y };
		InflateRect(&rect, tolerance);
		if (PointInRect (rect, dLocation))
			return { this, (int) i };
	}

	if (HitTestLine (zoomable, dLocation, tolerance, GetP0Coords(), GetP1Coords(), WireThickness))
		return { this, -1 };

	return { };
}

static const _bstr_t WireElementName = "Wire";

IXMLDOMElementPtr Wire::Serialize (IXMLDOMDocument* doc) const
{
	IXMLDOMElementPtr wireElement;
	HRESULT hr = doc->createElement (WireElementName, &wireElement); ThrowIfFailed(hr);

	hr = wireElement->appendChild(SerializeEnd(doc, _points[0]), nullptr); ThrowIfFailed(hr);
	hr = wireElement->appendChild(SerializeEnd(doc, _points[1]), nullptr); ThrowIfFailed(hr);

	return wireElement;
}

// static
unique_ptr<Wire> Wire::Deserialize (IProject* project, IXMLDOMElement* wireElement)
{
	IXMLDOMNodePtr firstChild;
	auto hr = wireElement->get_firstChild(&firstChild); ThrowIfFailed(hr);
	auto firstEnd = DeserializeEnd (project, (IXMLDOMElementPtr) firstChild);

	IXMLDOMNodePtr secondChild;
	hr = firstChild->get_nextSibling(&secondChild); ThrowIfFailed(hr);
	auto secondEnd = DeserializeEnd (project, (IXMLDOMElementPtr) secondChild);

	auto wire = make_unique<Wire>(firstEnd, secondEnd);
	return wire;
}

static const _bstr_t ConnectedEndString = "ConnectedEnd";
static const _bstr_t BridgeIndexString  = "BridgeIndex";
static const _bstr_t PortIndexString    = "PortIndex";
static const _bstr_t LooseEndString     = "LooseEnd";
static const _bstr_t XString            = "X";
static const _bstr_t YString            = "Y";

//static
IXMLDOMElementPtr Wire::SerializeEnd (IXMLDOMDocument* doc, const WireEnd& end)
{
	HRESULT hr;
	IXMLDOMElementPtr element;

	if (holds_alternative<ConnectedWireEnd>(end))
	{
		hr = doc->createElement (ConnectedEndString, &element); ThrowIfFailed(hr);
		auto port = get<ConnectedWireEnd>(end);
		auto& bridges = port->GetBridge()->GetProject()->GetBridges();
		auto it = find_if (bridges.begin(), bridges.end(), [port](auto& up) { return up.get() == port->GetBridge(); });
		auto bridgeIndex = it - bridges.begin();
		hr = element->setAttribute (BridgeIndexString, _variant_t(to_string(bridgeIndex).c_str())); ThrowIfFailed(hr);
		hr = element->setAttribute (PortIndexString, _variant_t(to_string(port->GetPortIndex()).c_str())); ThrowIfFailed(hr);
	}
	else if (holds_alternative<LooseWireEnd>(end))
	{
		auto& location = get<LooseWireEnd>(end);
		hr = doc->createElement (LooseEndString, &element); ThrowIfFailed(hr);
		hr = element->setAttribute (XString, _variant_t(location.x)); ThrowIfFailed(hr);
		hr = element->setAttribute (YString, _variant_t(location.y)); ThrowIfFailed(hr);
	}
	else
		throw not_implemented_exception();

	return element;
}

//static
WireEnd Wire::DeserializeEnd (IProject* project, IXMLDOMElement* element)
{
	HRESULT hr;

	_bstr_t name;
	hr = element->get_nodeName(name.GetAddress()); ThrowIfFailed(hr);

	if (wcscmp(name, ConnectedEndString) == 0)
	{
		_variant_t value;
		hr = element->getAttribute (BridgeIndexString, &value); ThrowIfFailed(hr);
		size_t bridgeIndex = wcstoul (value.bstrVal, nullptr, 10);
		hr = element->getAttribute (PortIndexString, &value); ThrowIfFailed(hr);
		size_t portIndex = wcstoul (value.bstrVal, nullptr, 10);
		return ConnectedWireEnd { project->GetBridges()[bridgeIndex]->GetPorts()[portIndex].get() };
	}
	else if (wcscmp (name, LooseEndString) == 0)
	{
		_variant_t value;
		hr = element->getAttribute (XString, &value); ThrowIfFailed(hr);
		float x = wcstof (value.bstrVal, nullptr);
		hr = element->getAttribute (YString, &value); ThrowIfFailed(hr);
		float y = wcstof (value.bstrVal, nullptr);
		return LooseWireEnd { x, y };
	}
	else
		throw not_implemented_exception();
}


