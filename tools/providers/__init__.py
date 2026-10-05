import importlib
import pkgutil


DISABLED_PROVIDERS = {
    "bitsearch",
    "cloudtorrents",
    # "knaben",
    "limetorrents",
    "rarbg",
    "the_pirate_bay",
    "torrentquest",
}


def load_providers():
    providers = []

    for module in pkgutil.iter_modules(__path__):
        if module.name.startswith("_"):
            continue

        if module.name in DISABLED_PROVIDERS:
            continue

        mod = importlib.import_module(f"{__name__}.{module.name}")
        provider = getattr(mod, "PROVIDER", None)

        if provider is not None:
            providers.append(provider)

    return providers