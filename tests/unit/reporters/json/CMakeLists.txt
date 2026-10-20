#SPDX - License - Identifier : GPL - 3.0 - or -later
#Test fragment(ADR 0004) : tier T1 of src / reporters / json.
loadforge_test_suite(loadforge_unit_reporters_json_tests SOURCES
                         json_text_test.cpp json_writer_test.cpp LINK loadforge_reporters_json
                             loadforge_controller_results loadforge_controller)
