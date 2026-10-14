# The library comes in as a component (main/idf_component.yml); its manifest dependency is a requirement of main.
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer)
