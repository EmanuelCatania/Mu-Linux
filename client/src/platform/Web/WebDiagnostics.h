#pragma once
#include "platform/IDiagnostics.h"
namespace Platform {
// Navegador: no hay señales ni backtrace nativo (el navegador muestra la pila
// del wasm en la consola) ni contador de ciclos para medir la CPU.
class WebDiagnostics final : public IDiagnostics {
public:
    bool Install(DiagnosticLog) override { return true; }
    std::int64_t MeasureClockRate(std::uint32_t) override { return 0; }
};
}
