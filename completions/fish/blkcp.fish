# Fish shell completion for blkcp

complete -c blkcp -s i -l input -d "Input file or block device" -r -F
complete -c blkcp -s o -l output -d "Output file or block device" -r -F
complete -c blkcp -s b -l block-size -d "Block size (e.g. 512, 64K, 1M, 4M, auto)" -x -a "auto 512 4096 64K 1M 4M 8M"
complete -c blkcp -l bs -d "Block size alias" -x -a "auto 512 4096 64K 1M 4M 8M"
complete -c blkcp -s e -l engine -d "Execution backend engine" -x -a "auto uring async reflink splice sync"
complete -c blkcp -s l -l limit -d "Exact byte transfer limit" -x
complete -c blkcp -s s -l size -d "Exact byte transfer limit alias" -x
complete -c blkcp -l bytes -d "Exact byte transfer limit alias" -x
complete -c blkcp -s c -l count -d "Number of blocks to transfer" -x
complete -c blkcp -s p -l progress -d "Display real-time transfer progress"
complete -c blkcp -l json -d "Emit machine-readable NDJSON telemetry"
complete -c blkcp -l queue-depth -d "Async ringbuffer queue depth (2..1024 slots)" -x
complete -c blkcp -s q -l quiet -d "Suppress non-fatal output"
complete -c blkcp -s f -l force -d "Override Target Safety Guard"
complete -c blkcp -s n -l dry-run -d "Simulate transfer and show plan without writing"
complete -c blkcp -l autotune -d "Enable dynamic throughput autotuning"
complete -c blkcp -l hash -d "Compute streaming SHA-256 checksum"
complete -c blkcp -l sha256 -d "Compute streaming SHA-256 checksum"
complete -c blkcp -l direct -d "Use Direct I/O (O_DIRECT)"
complete -c blkcp -l nocache -d "Evict page cache sequentially"
complete -c blkcp -l sparse -d "Punch holes for zero blocks"
complete -c blkcp -l sync -d "Pad short reads with zero bytes"
complete -c blkcp -l swab -d "Swap adjacent byte pairs"
complete -c blkcp -l skip -d "Skip bytes of input" -x
complete -c blkcp -l seek -d "Seek bytes of output" -x
complete -c blkcp -l fdatasync -d "Flush output data before completion"
complete -c blkcp -l fsync -d "Flush output data and metadata before completion"
complete -c blkcp -l noerror -d "Continue across read errors"
complete -c blkcp -l notrunc -d "Do not truncate output file"
complete -c blkcp -s h -l help -d "Display usage help"
complete -c blkcp -s v -l version -d "Display version information"
