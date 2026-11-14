# Batch15：儀器延遲使嚴格unity讀回失敗；保留為未完成實驗

2026-11-13。來源camera-writer-window/v1，observer camera-writer-trace/v1。單次部署357012 bytes，SHA256 aa64980bb6295ea41a546d4d0e9ecb0a799e62493bb660a147f36246773e002c。13份原件775491 bytes全部通過長度/CRC32/SHA256；實際3DSX、C源碼、host-source及雜湊均保存。未重寫原件或把complete0改為成功。

現機Camera PID23、完整86016 bytes text SHA256 d599b6b685b1888b1e33c94ca54c4325719d371a697bb8b51b3481b16f5ead2f，與存檔及六個入口指令相符。writer-audit/live-identity-01另有單次唯讀核對，約2.583秒後明確detach成功。

本輪準備與歸一成功、GO後首個DMA完成且同一DMA handle已關閉。觀察器記錄204個syscall stop，52對I2C請求/回覆：command0xA讀40次、command8寫12次；四個IFP128寫入均捕捉，對應I2C service reply word1均0。然而每通道寫入與讀回被GDB暫停拉長，四筆immediate讀回已全133（32D4/6/8/A時間戳4003590634344/36056/37258/38929）。裝置v1契約要求嚴格128，因此未進入三次VSYNC觀察，NO_RECEIVE_WINDOW complete0 observations0，正確停止。不能據此排除Camera寫入者或宣稱ISP控制成功。

Camera還有背景讀364E活動，已保存為真正外部I2C請求，未將背景服務當作停用。現有已捕捉區間沒有寫133，但缺少完整後續觀察和明確追蹤收尾，所以只保留此局部事實。

觀察器收尾另有RSP封包競態：送interrupt時已有T05 syscall stop在途；其後S02被誤當成QCatchSyscalls:0命令回覆，導致未明確關閉catch/detach。trace-status.json如實detached=false、catch_disabled=false、passed=false。連線最終已關閉，Luma的斷線清理讓裝置繼續執行；裝置原件中PREVIEW/AWB/SEQ恢復、恢復Refresh及物理控制恢復均成功（WRITER_WINDOW_END complete0 restored1），全部RAM原件已回收。不能把明確detach缺失改寫成成功。

另發現這版kernel syscall-return debug事件的register0常保留地址/參數等非零值，不能直接命名為IPC transport Result；原始封包及v1欄位保持不改。新v2將它保存為register_r0原始值，以IPC response word1和裝置call結果判斷服務成功，不將非零原始CPU暫存器當作錯誤碼。

後續先登記獨立camera-writer-window/v2、trace/v2及report/v2：預先在未追蹤時驗證四通道128；追蹤下仍僅一次四通道128寫入且嚴格要求寫入與I2C服務成功，隨後物理讀回作1..512原始觀察，其他控制與新VSYNC證據不放寬。這是writer觀察實驗，不是unity/校準通過。舊v1和no-receive/unity來源仍會在相同133讀回時失敗。

RSP v2在不重發命令的情況下，按協定排空最多8個在途非同步stop/output封包，且即使關閉catch失敗也嘗試detach；增加真實TCP/RSP回歸。這是已登記的傳輸協定處理，沒有重拍、隱藏補寫或fallback。

Runtime分類：獨立hardware diagnostic compute，明示standalone_hardware_diagnostic override，僅local_trace；Film3DS CONTRACT_MATRIX先登記來源、同步gate、限額、錯誤處理與觀察器，再實作與測試。新host觀察器內部狀態已在該矩陣列明；未改Vision矩陣、verifier、runtime_internal_fields、portable metadata或frontend。已執行C真實socket情境、CLI、TCP/RSP端到端與codec、identity/報告器正負例、既有NoReceive/ISP/sweep回歸及ARM建置驗證；對應日誌在父目錄writer-window-*。Vision三項pytest未跑，因Film3DS無對應檔案且未改其runtime邊界：

```sh
pytest tests/test_runtime_contract_boundaries.py -q
pytest tests/test_runtime_projection.py -q
pytest tests/test_metadata_runtime_package.py -q
```

實機命令：

```sh
python3 tools/exposure_sweep_session.py 172.20.10.2 --writer-window --out build/hardware/exposure-sweep/batch15-camera-writer-trace
python3 tools/camera_writer_trace.py 172.20.10.2 --expected build/hardware/exposure-sweep/writer-audit/expected-camera-identity.json --out build/hardware/exposure-sweep/batch15-camera-writer-trace/trace
```

執行時v1完整腳本已凍結在host-source，以上命令現行預設已指向另行登記的v2，重現v1需使用凍結原件。未聲稱全局外部I2C監聽，尚未識別133實際寫入者。
