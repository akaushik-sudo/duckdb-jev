#define DUCKDB_EXTENSION_MAIN

#include "snx_jev_extension.hpp"

#include "jev_client.hpp"
#include "jev_config.hpp"
#include "jev_functions.hpp"

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("Ask your DuckDB tables questions in plain language");
	JevConfig::RegisterSettings(DBConfig::GetConfig(loader.GetDatabaseInstance()));
	JevRegisterFunctions(loader);
}

void SnxJevExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SnxJevExtension::Name() {
	return "snx_jev";
}

std::string SnxJevExtension::Version() const {
#ifdef EXT_VERSION_SNX_JEV
	return EXT_VERSION_SNX_JEV;
#else
	return JEV_VERSION;
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(snx_jev, loader) {
	duckdb::LoadInternal(loader);
}
}
