#include "stdafx.h"
#include "platform/ITimer.h"
#include "game/item/ChaosMixRates.h"
#include "net/Network.h"
#include "ui/UIState.h"

CChaosMixRates gChaosMixRates;

void CChaosMixRates::Reset()
{
    *this = CChaosMixRates{};
}

void CChaosMixRates::Update(int type, const ITEM* items)
{
    // Esperar el ACK del movimiento: la vista local puede ser provisional.
    if (!items || !UIState::CanQueryChaosRate()) {
        if (UIState::IsMixBusy() && m_State == State::Ready) {
            m_State = State::Idle;
            m_Attempts = 0;
        }
        return;
    }
    bool changed = (type != m_Type);
    m_Type = type;
    for (int i = 0; i < 32; ++i) {
        Material current;
        current.Type = items[i].Type;
        if (current.Type != -1) {
            current.Level = items[i].Level;
            current.Durability = items[i].Durability;
            current.Option = items[i].Option1;
        }
        const Material& old = m_Items[i];
        changed |= current.Type != old.Type || current.Level != old.Level ||
                   current.Durability != old.Durability || current.Option != old.Option;
        m_Items[i] = current;
    }
    if (changed) {
        ++m_Revision;
        if (m_State != State::Waiting) m_State = State::Idle;
        m_Attempts = 0;
    }
    // Se consulta con cualquier contenido: la receta la reconoce el server, y
    // uno anterior responde 0 a los tipos que no calcula.  Sin consultas por frame.
    bool hasItems = false;
    for (int i = 0; i < 32 && !hasItems; ++i) hasItems = m_Items[i].Type != -1;
    if (!hasItems || m_State != State::Idle || m_Attempts >= 3) return;
    const DWORD now = Platform::GetTimer().Ticks();
    if (m_Attempts && now - m_LastSent < 1000) return;
    Proto::PMSG_CHAOS_MIX_RATE_RECV packet{};
    packet.header = { 0xC1, sizeof(packet), 0x88 };
    packet.type = type;
    m_State = State::Waiting;
    m_LastSent = now;
    ++m_Attempts;
    m_PendingRevision = m_Revision;
    gNetwork.SendC1((const BYTE*)&packet, sizeof(packet));
}

void CChaosMixRates::Receive(const Proto::PMSG_CHAOS_MIX_RATE_SEND& packet, bool hasType, int type, const Recipe* recipe)
{
    if (m_State != State::Waiting) return;
    m_State = State::Idle;
    // La respuesta no trae receta: una sola consulta pendiente, por revisiÃ³n.
    if (!UIState::CanQueryChaosRate() || m_PendingRevision != m_Revision) return;
    // 0x88 no tiene ID: drenar la respuesta antes de reintentar, aun si venció.
    if (Platform::GetTimer().Ticks() - m_LastSent > 5000) return;
    // La UI existente imprime enteros con signo; no aceptar valores imposibles.
    if (packet.rate > 100 || packet.money > 0x7FFFFFFFu) return;
    // Las recetas de CheckMixRecipe caben en un byte con signo (el 0x86 lo
    // manda como BYTE).
    if (hasType && (type < -128 || type > 127)) return;
    m_Rate = (int)packet.rate;
    m_Money = (int)packet.money;
    m_HasServerType = hasType;
    m_ServerType = hasType ? type : 0;
    m_HasRecipe = recipe != nullptr && hasType;
    m_Recipe = m_HasRecipe ? *recipe : Recipe{};
    m_State = State::Ready;
}

const CChaosMixRates::Recipe* CChaosMixRates::GetRecipe() const
{
    if (!m_HasRecipe || m_State != State::Ready || !UIState::CanQueryChaosRate()) return nullptr;
    return (int)MixType == m_ServerType ? &m_Recipe : nullptr;
}

int CChaosMixRates::ResolveType(int localType) const
{
    if (m_State == State::Ready && m_HasServerType && UIState::CanQueryChaosRate())
        return m_ServerType;
    return localType;
}

bool CChaosMixRates::Get(int& rate, int& money) const
{
    if (m_State != State::Ready || !UIState::CanQueryChaosRate()) return false;
    if ((int)MixType != (m_HasServerType ? m_ServerType : m_Type)) return false;
    rate = m_Rate;
    money = m_Money;
    return true;
}
