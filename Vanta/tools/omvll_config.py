# O-MVLL configuration for Nexa (Layer B). Loaded via OMVLL_CONFIG when the build
# is wired to the prebuilt plugin — see OBFUSCATION.md. Cold code gets the heavy
# passes; hot paths (the VM, hook trampolines, metamethods) are left fast.
import omvll
from functools import lru_cache

HOT = ("Pump", "hook", "namecall", "metamethod", "taskscheduler", "executor",
       "trampoline", "Reflection")

class NexaConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()

    def _is_hot(self, func):
        n = func.name or ""
        return any(h.lower() in n.lower() for h in HOT)

    def obfuscate_string(self, _mod, _func, _string):
        return True  # encrypt every string literal that survives Layer A

    def flatten_cfg(self, _mod, func):
        return not self._is_hot(func)          # CFF cold code only

    def obfuscate_arithmetic(self, _mod, func, _info):
        return not self._is_hot(func)

    def anti_hooking(self, _mod, func, _info):
        return not self._is_hot(func)

@lru_cache(maxsize=1)
def omvll_get_config():
    return NexaConfig()
