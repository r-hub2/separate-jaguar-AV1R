# TODO

## In progress
- [ ] Vulkan GPU encode path (`av1r_encode_vulkan.cpp`) — pipeline написан, не протестирован на железе

## Planned
- [ ] Тест encode на реальном TIFF стеке (нужен тестовый файл)
- [ ] Windows поддержка (Makevars.win.in)
- [ ] CRAN submission

## Done
- [x] CPU путь через ffmpeg (system2)
- [x] Auto-detect backend (Vulkan / CPU)
- [x] R CMD check: no ERRORs, no WARNINGs
- [x] README
- [x] Тесты: options, backend, convert, Vulkan
