// Test fixture: a shared library that exports no slopkit_plugin_entry symbol.
// The loader must reject it with a diagnostic instead of aborting.

[[maybe_unused]] int slopkit_fixture_no_entry()
{
    return 0;
}
