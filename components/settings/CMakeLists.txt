idf_component_register(
    SRCS "settings_store.cpp"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "${CMAKE_SOURCE_DIR}/data/profiles.json")
