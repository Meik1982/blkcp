use crate::args::{Engine, HashAlgo};
use crate::stats::{format_speed, human_readable_bytes};
use crate::tui_device::{format_bytes_human, EndpointKind};
use crate::tui_state::{FocusPane, ModalDialog, SyncMode, TuiState};

use ratatui::{
    layout::{Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Cell, Clear, Gauge, Paragraph, Row, Table},
    Frame,
};
use std::sync::atomic::Ordering;

pub fn render_ui(f: &mut Frame, state: &TuiState) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3), // 0: Header & Presets
            Constraint::Min(9),    // 1: Source & Target Endpoints
            Constraint::Length(9), // 2: 16-Module Matrix
            Constraint::Length(5), // 3: Live Progress / Combination Advisor & CLI
            Constraint::Length(3), // 4: Footer Telemetry & Controls
        ])
        .split(f.area());

    // 0. Header
    let header = Paragraph::new(Line::from(vec![
        Span::styled(
            " blkcp-tui v2.0 ",
            Style::default()
                .fg(Color::Black)
                .bg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        ),
        Span::raw(" Presets: "),
        Span::styled("[1] NVMe-Uring ", Style::default().fg(Color::Cyan)),
        Span::styled("[2] Forensik+BLKS ", Style::default().fg(Color::Green)),
        Span::styled("[3] Sparse+B3 ", Style::default().fg(Color::Magenta)),
        Span::styled("[4] Reflink ", Style::default().fg(Color::Blue)),
        Span::styled("[5] 2G-Bench ", Style::default().fg(Color::Yellow)),
        Span::styled("[6] Splice ", Style::default().fg(Color::LightBlue)),
        Span::styled("[7] Direct-NoCache ", Style::default().fg(Color::LightCyan)),
        Span::styled("[8] Swab ", Style::default().fg(Color::LightMagenta)),
        Span::styled(
            " | [Z] Auto-Fix ",
            Style::default()
                .fg(Color::White)
                .add_modifier(Modifier::BOLD),
        ),
    ]))
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" Modulare Block-Replikations & Krypto-Pipeline "),
    );
    f.render_widget(header, chunks[0]);

    // 1. Source & Target Tables
    let center_chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(50), Constraint::Percentage(50)])
        .split(chunks[1]);

    let source_rows: Vec<Row> = state
        .devices
        .iter()
        .enumerate()
        .map(|(idx, d)| {
            let is_sel = idx == state.source_idx;
            let prefix = if is_sel { "▶ " } else { "  " };
            let mut style = Style::default();
            if is_sel {
                style = style.fg(Color::Yellow).add_modifier(Modifier::BOLD);
            } else if d.kind == EndpointKind::VirtualDevice {
                style = style.fg(Color::Cyan);
            } else if d.kind == EndpointKind::RegularFile {
                style = style.fg(Color::Green);
            }
            Row::new(vec![
                Cell::from(format!("{}{}", prefix, d.path.display())),
                Cell::from(d.size_human()),
                Cell::from(d.model.clone()),
            ])
            .style(style)
        })
        .collect();

    let source_table = Table::new(
        source_rows,
        [
            Constraint::Length(18),
            Constraint::Length(12),
            Constraint::Min(16),
        ],
    )
    .header(
        Row::new(vec!["Quelle (-i)", "Größe", "Typ / Modell"]).style(
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ),
    )
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" [Quelle] ([I] Pfad, [F] Browser, [P] Partitionen) ")
            .border_style(if state.focus == FocusPane::Source {
                Style::default().fg(Color::Yellow)
            } else {
                Style::default()
            }),
    );
    f.render_widget(source_table, center_chunks[0]);

    let target_rows: Vec<Row> = state
        .devices
        .iter()
        .enumerate()
        .map(|(idx, d)| {
            let is_sel = idx == state.target_idx;
            let prefix = if is_sel { "▶ " } else { "  " };
            let mut style = Style::default();
            if is_sel {
                style = style.fg(Color::Yellow).add_modifier(Modifier::BOLD);
            }
            if d.is_system_root {
                style = style.fg(Color::Red).add_modifier(Modifier::BOLD);
            } else if !d.mount_points.is_empty() {
                style = style.fg(Color::Magenta);
            } else if d.kind == EndpointKind::VirtualDevice {
                style = style.fg(Color::Cyan);
            } else if d.kind == EndpointKind::RegularFile {
                style = style.fg(Color::Green);
            }

            let status_str = if d.is_system_root {
                "SYSTEM / ROOT (SPERRE)".to_string()
            } else if !d.mount_points.is_empty() {
                format!("Mounted: {}", d.mount_points.join(","))
            } else {
                d.model.clone()
            };

            Row::new(vec![
                Cell::from(format!("{}{}", prefix, d.path.display())),
                Cell::from(d.size_human()),
                Cell::from(status_str),
            ])
            .style(style)
        })
        .collect();

    let target_table = Table::new(
        target_rows,
        [
            Constraint::Length(18),
            Constraint::Length(12),
            Constraint::Min(16),
        ],
    )
    .header(
        Row::new(vec!["Ziel (-o)", "Größe", "Safety / Status"]).style(
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ),
    )
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" [Ziel] ([O] Pfad, [F] Browser, Rot = Geschützt) ")
            .border_style(if state.focus == FocusPane::Target {
                Style::default().fg(Color::Yellow)
            } else {
                Style::default()
            }),
    );
    f.render_widget(target_table, center_chunks[1]);

    // 2. 16-Module Combination Matrix
    render_module_matrix(f, state, chunks[2]);

    // 3. Live Progress OR Combination Advisor + CLI Command
    if let Some(ref job) = state.active_job {
        let bytes = job.stats.get_bytes();
        let elapsed = job.stats.start_time.elapsed().as_secs_f64().max(0.000_001);
        let (spd, unit) = format_speed(bytes as f64 / elapsed);
        let r_in = job.stats.records_in_full.load(Ordering::Relaxed);
        let r_out = job.stats.records_out_full.load(Ordering::Relaxed);

        let ratio = if let Some(exp) = job.expected_bytes {
            if exp > 0 {
                (bytes as f64 / exp as f64).clamp(0.0, 1.0)
            } else {
                0.5
            }
        } else {
            0.5
        };

        let label = format!(
            "{} kopiert | {:.1} {} | {:.1} s | Blöcke In: {} Out: {} | [X/Esc] Abbruch",
            human_readable_bytes(bytes),
            spd,
            unit,
            elapsed,
            r_in,
            r_out
        );

        let gauge = Gauge::default()
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" LIVE TRANSFER TELEMETRIE ")
                    .border_style(Style::default().fg(Color::Green)),
            )
            .gauge_style(
                Style::default()
                    .fg(Color::Green)
                    .bg(Color::Black)
                    .add_modifier(Modifier::BOLD),
            )
            .ratio(ratio)
            .label(label);
        f.render_widget(gauge, chunks[3]);
    } else {
        let (advice, adv_color) = state.analyze_combination();
        let cli_cmd = state.build_cli_command();

        let third_line = if let Some((ref digest, algo)) = state.last_hash_result {
            Line::from(vec![
                Span::styled(
                    format!(" Letzter {}-Digest: ", algo.to_uppercase()),
                    Style::default()
                        .fg(Color::Green)
                        .add_modifier(Modifier::BOLD),
                ),
                Span::styled(digest.clone(), Style::default().fg(Color::White)),
            ])
        } else if let Some(ref sum) = state.last_summary_line {
            Line::from(Span::styled(
                format!(" {}", sum),
                Style::default().fg(Color::LightGreen),
            ))
        } else {
            Line::from(vec![
                Span::styled(
                    " Äquivalenter CLI-Befehl: ",
                    Style::default().fg(Color::Cyan),
                ),
                Span::styled(cli_cmd, Style::default().fg(Color::White)),
            ])
        };

        let advisor_lines = vec![
            Line::from(vec![
                Span::styled(
                    " Kombination-Advisor: ",
                    Style::default()
                        .fg(adv_color)
                        .add_modifier(Modifier::BOLD),
                ),
                Span::styled(advice, Style::default().fg(Color::White)),
            ]),
            Line::from(vec![
                Span::styled(" Quick-Actions: ", Style::default().fg(Color::Yellow)),
                Span::styled(
                    "[Enter] Starten  |  [N] Dry-Run Simulation  |  [F] Datei-Browser  |  [L] Offsets/Limits  |  [R] Geräte-Refresh",
                    Style::default().fg(Color::DarkGray),
                ),
            ]),
            third_line,
        ];

        let advisor = Paragraph::new(advisor_lines).block(
            Block::default()
                .borders(Borders::ALL)
                .title(" Hardware- & Modul-Advisor (Validierung & CLI-Export) ")
                .border_style(Style::default().fg(adv_color)),
        );
        f.render_widget(advisor, chunks[3]);
    }

    // 4. Footer Telemetry & Status
    let footer_text = vec![Line::from(vec![
        Span::styled(
            format!(" Status: {} ", state.status_msg),
            Style::default().fg(Color::Yellow),
        ),
        Span::raw(" | [Tab/Shift+Tab] Fokus  [Leertaste] Umschalten  [Q] Beenden"),
    ])];

    let footer = Paragraph::new(footer_text).block(
        Block::default()
            .borders(Borders::ALL)
            .title(" Statusleiste "),
    );
    f.render_widget(footer, chunks[4]);

    // Modale Dialoge
    render_modal(f, state);
}

pub fn render_module_matrix(f: &mut Frame, state: &TuiState, area: Rect) {
    let eng_name = match state.engine {
        Engine::Auto => "auto (smart)",
        Engine::Sync => "sync (blocking)",
        Engine::Async => "async (ringbuf)",
        Engine::Reflink => "reflink (cow)",
        Engine::Uring => "io_uring (async)",
        Engine::Splice => "splice (pipe-zero)",
    };

    let hash_name = match state.hash_algo {
        HashAlgo::None => "aus",
        HashAlgo::Blake3 => "BLAKE3 (SIMD)",
        HashAlgo::Blks => "BLKS-384 (PQ)",
        HashAlgo::Sha256 => "SHA-256",
    };
    let qd_name = state
        .queue_depth_opt
        .map(|q| q.to_string())
        .unwrap_or_else(|| "auto".to_string());

    let items: [(usize, &str, String, bool); 16] = [
        (0, "[B] Blockgröße", state.block_size_str.to_string(), true),
        (1, "[E] I/O-Engine", eng_name.to_string(), true),
        (
            2,
            "[H] Krypto-Hash",
            hash_name.to_string(),
            state.hash_algo != HashAlgo::None,
        ),
        (
            3,
            "[J] Shards/Threads",
            format!("{}", state.threads),
            state.threads > 1,
        ),
        (
            4,
            "[U] Queue-Depth",
            qd_name,
            state.queue_depth_opt.is_some(),
        ),
        (
            5,
            "[D] Direct I/O",
            if state.direct_io { "AN" } else { "aus" }.to_string(),
            state.direct_io,
        ),
        (
            6,
            "[C] No-Cache",
            if state.nocache { "AN" } else { "aus" }.to_string(),
            state.nocache,
        ),
        (
            7,
            "[A] Sparse-Holes",
            if state.sparse { "AN" } else { "aus" }.to_string(),
            state.sparse,
        ),
        (
            8,
            "[W] AVX2 Swab",
            if state.swab { "AN" } else { "aus" }.to_string(),
            state.swab,
        ),
        (
            9,
            "[Y] Sync-Pad",
            if state.sync_pad { "AN" } else { "aus" }.to_string(),
            state.sync_pad,
        ),
        (
            10,
            "[T] No-Truncate",
            if state.notrunc { "AN" } else { "aus" }.to_string(),
            state.notrunc,
        ),
        (
            11,
            "[G] Flush-Sync",
            state.sync_mode.as_str().to_string(),
            state.sync_mode != SyncMode::None,
        ),
        (
            12,
            "[M] Ignore-Err",
            if state.noerror { "AN" } else { "aus" }.to_string(),
            state.noerror,
        ),
        (
            13,
            "[N] Dry-Run",
            if state.dry_run { "AN" } else { "aus" }.to_string(),
            state.dry_run,
        ),
        (
            14,
            "[L] Limit/Offsets",
            format!(
                "L:{} S:{}/{}",
                if state.limit_str.is_empty() {
                    "∞"
                } else {
                    &state.limit_str
                },
                state.skip_str,
                state.seek_str
            ),
            !state.limit_str.is_empty() && state.limit_str != "0",
        ),
        (
            15,
            "[Force] Guard-Bypass",
            if state.force_override { "AN!" } else { "aus" }.to_string(),
            state.force_override,
        ),
    ];

    let mut rows = Vec::new();
    for r in 0..4 {
        let mut cells = Vec::new();
        for c in 0..4 {
            let idx = r + c * 4;
            let (i, label, val, active) = &items[idx];
            let is_sel = state.focus == FocusPane::Modules && state.module_idx == *i;

            let mut style = Style::default();
            if is_sel {
                style = style
                    .bg(Color::DarkGray)
                    .fg(Color::Yellow)
                    .add_modifier(Modifier::BOLD);
            } else if *active {
                style = style.fg(Color::Green);
            } else {
                style = style.fg(Color::Gray);
            }

            let cell_text = format!("{}: {}", label, val);
            cells.push(Cell::from(cell_text).style(style));
        }
        rows.push(Row::new(cells));
    }

    let matrix_table = Table::new(
        rows,
        [
            Constraint::Percentage(25),
            Constraint::Percentage(25),
            Constraint::Percentage(25),
            Constraint::Percentage(25),
        ],
    )
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" 16-Modul Matrix ([Tab] Fokus, [↑↓←→] Nav, [Leertaste] Ändern, [Direct-Keys]) ")
            .border_style(if state.focus == FocusPane::Modules {
                Style::default().fg(Color::Yellow)
            } else {
                Style::default()
            }),
    );
    f.render_widget(matrix_table, area);
}

pub fn render_modal(f: &mut Frame, state: &TuiState) {
    match &state.modal {
        ModalDialog::None => {}
        ModalDialog::SafetyConfirm => {
            let area = centered_rect(60, 25, f.area());
            f.render_widget(Clear, area);

            let dst_name = state
                .devices
                .get(state.target_idx)
                .map(|d| d.path.display().to_string())
                .unwrap_or_else(|| "Unbekannt".to_string());

            let text = vec![
                Line::from(Span::styled(
                    "ACHTUNG: SCHREIBZUGRIFF AUF BLOCKGERÄT / ZIEL!",
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                )),
                Line::from(""),
                Line::from(format!("Bist du sicher, dass du auf '{}' schreiben willst?", dst_name)),
                Line::from("Alle bestehenden Daten auf dem Ziel können unwiderruflich überschrieben werden."),
                Line::from(""),
                Line::from(vec![
                    Span::styled("[Y] / [Enter] Ja, Schreiben starten  ", Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)),
                    Span::styled("[N] / [Esc] Abbrechen", Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)),
                ]),
            ];

            let modal = Paragraph::new(text).block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" SICHERHEITS-BESTÄTIGUNG ")
                    .border_style(Style::default().fg(Color::Red)),
            );
            f.render_widget(modal, area);
        }
        ModalDialog::PathInput { for_target, buffer } => {
            let area = centered_rect(60, 20, f.area());
            f.render_widget(Clear, area);

            let title = if *for_target {
                " Benutzerdefinierten Zielpfad eingeben (-o) "
            } else {
                " Benutzerdefinierten Quellpfad eingeben (-i) "
            };

            let text = vec![
                Line::from(format!("Pfad: {}█", buffer)),
                Line::from(""),
                Line::from("[Enter] Übernehmen  |  [Esc] Abbrechen  |  [F] Datei-Browser"),
            ];

            let modal = Paragraph::new(text).block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(title)
                    .border_style(Style::default().fg(Color::Yellow)),
            );
            f.render_widget(modal, area);
        }
        ModalDialog::LimitInput {
            field,
            limit_buf,
            skip_buf,
            seek_buf,
        } => {
            let area = centered_rect(65, 30, f.area());
            f.render_widget(Clear, area);

            let sel_style = Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD);
            let norm_style = Style::default().fg(Color::White);

            let text = vec![
                Line::from(Span::styled(
                    "Konfiguration von Byte-Limits und Stream-Offsets:",
                    Style::default().add_modifier(Modifier::BOLD),
                )),
                Line::from(""),
                Line::from(vec![
                    Span::styled(
                        " [L] Limit (-l / --limit): ",
                        if *field == 0 { sel_style } else { norm_style },
                    ),
                    Span::raw(format!("{} (z. B. 500M, 4G, auto)", limit_buf)),
                ]),
                Line::from(vec![
                    Span::styled(
                        " [S] Skip  (--skip, In) : ",
                        if *field == 1 { sel_style } else { norm_style },
                    ),
                    Span::raw(format!(
                        "{} (Bytes/Blöcke vor Lese-Start überspringen)",
                        skip_buf
                    )),
                ]),
                Line::from(vec![
                    Span::styled(
                        " [K] Seek  (--seek, Out): ",
                        if *field == 2 { sel_style } else { norm_style },
                    ),
                    Span::raw(format!("{} (Offset auf Ausgabedatei anspringen)", seek_buf)),
                ]),
                Line::from(""),
                Line::from("[Tab / ↑↓] Feld wechseln  |  [Enter] Speichern  |  [Esc] Abbrechen"),
            ];

            let modal = Paragraph::new(text).block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" Byte-Limits & Offset-Planung ")
                    .border_style(Style::default().fg(Color::Yellow)),
            );
            f.render_widget(modal, area);
        }
        ModalDialog::FileBrowser {
            for_target,
            current_dir,
            entries,
            selected_idx,
        } => {
            let area = centered_rect(75, 65, f.area());
            f.render_widget(Clear, area);
            let title = format!(
                " Datei-Browser ({}) - {} ",
                if *for_target { "Ziel -o" } else { "Quelle -i" },
                current_dir.display()
            );

            let start = selected_idx.saturating_sub(10);
            let rows: Vec<Row> = entries
                .iter()
                .enumerate()
                .skip(start)
                .take(15)
                .map(|(idx, e)| {
                    let is_sel = idx == *selected_idx;
                    let style = if is_sel {
                        Style::default()
                            .fg(Color::Yellow)
                            .add_modifier(Modifier::BOLD)
                    } else if e.is_dir {
                        Style::default().fg(Color::Cyan)
                    } else {
                        Style::default().fg(Color::White)
                    };
                    Row::new(vec![
                        Cell::from(format!("{}{}", if is_sel { "▶ " } else { "  " }, e.name)),
                        Cell::from(if e.is_dir {
                            "<DIR>".to_string()
                        } else {
                            format_bytes_human(e.size_bytes)
                        }),
                    ])
                    .style(style)
                })
                .collect();

            let table = Table::new(rows, [Constraint::Min(40), Constraint::Length(16)])
                .header(
                    Row::new(vec![
                        "Name ([Enter] Öffnen/Wählen, [Esc] Schließen)",
                        "Größe",
                    ])
                    .style(Style::default().fg(Color::Green)),
                )
                .block(
                    Block::default()
                        .borders(Borders::ALL)
                        .title(title)
                        .border_style(Style::default().fg(Color::Yellow)),
                );
            f.render_widget(table, area);
        }
    }
}

pub fn centered_rect(percent_x: u16, percent_y: u16, r: Rect) -> Rect {
    let popup_layout = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Percentage((100 - percent_y) / 2),
            Constraint::Percentage(percent_y),
            Constraint::Percentage((100 - percent_y) / 2),
        ])
        .split(r);

    Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage((100 - percent_x) / 2),
            Constraint::Percentage(percent_x),
            Constraint::Percentage((100 - percent_x) / 2),
        ])
        .split(popup_layout[1])[1]
}
