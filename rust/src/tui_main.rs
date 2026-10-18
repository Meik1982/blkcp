mod tui_device;

use crossterm::{
    event::{self, Event, KeyCode},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{
    backend::CrosstermBackend,
    layout::{Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Cell, Clear, Paragraph, Row, Table},
    Terminal,
};
use std::io;
use std::path::PathBuf;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FocusPane {
    Source,
    Target,
    Options,
}

#[allow(dead_code)]
struct TuiState {
    devices: Vec<tui_device::BlockDevice>,
    source_idx: usize,
    target_idx: usize,
    source_custom_file: Option<PathBuf>,
    focus: FocusPane,
    block_size: &'static str,
    engine: &'static str,
    hash_algo: &'static str,
    direct_io: bool,
    swab: bool,
    sparse: bool,
    show_safety_modal: bool,
    running: bool,
    status_msg: String,
}

impl TuiState {
    fn new() -> Self {
        let devices = tui_device::scan_block_devices();
        Self {
            devices,
            source_idx: 0,
            target_idx: 0,
            source_custom_file: None,
            focus: FocusPane::Target,
            block_size: "1M",
            engine: "auto",
            hash_algo: "blake3",
            direct_io: false,
            swab: false,
            sparse: false,
            show_safety_modal: false,
            running: true,
            status_msg: "Bereit. Wähle Quelle und Ziel aus.".to_string(),
        }
    }

    fn refresh_devices(&mut self) {
        self.devices = tui_device::scan_block_devices();
        if self.source_idx >= self.devices.len() {
            self.source_idx = 0;
        }
        if self.target_idx >= self.devices.len() {
            self.target_idx = 0;
        }
        self.status_msg = format!(
            "Laufwerke aktualisiert: {} Geräte gefunden",
            self.devices.len()
        );
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let mut state = TuiState::new();

    while state.running {
        terminal.draw(|f| {
            render_ui(f, &state);
        })?;

        if let Event::Key(key) = event::read()? {
            if state.show_safety_modal {
                match key.code {
                    KeyCode::Char('y') | KeyCode::Char('Y') | KeyCode::Enter => {
                        state.show_safety_modal = false;
                        state.status_msg =
                            "Schreibvorgang freigegeben! Starte Übertragung...".to_string();
                    }
                    KeyCode::Char('n') | KeyCode::Char('N') | KeyCode::Esc => {
                        state.show_safety_modal = false;
                        state.status_msg = "Schreibvorgang vom Benutzer abgebrochen.".to_string();
                    }
                    _ => {}
                }
                continue;
            }

            match key.code {
                KeyCode::Char('q') | KeyCode::Esc => state.running = false,
                KeyCode::Tab => {
                    state.focus = match state.focus {
                        FocusPane::Source => FocusPane::Target,
                        FocusPane::Target => FocusPane::Options,
                        FocusPane::Options => FocusPane::Source,
                    };
                }
                KeyCode::Char('r') | KeyCode::Char('R') => {
                    state.refresh_devices();
                }
                KeyCode::Up | KeyCode::Char('k') => match state.focus {
                    FocusPane::Source if state.source_idx > 0 => {
                        state.source_idx -= 1;
                    }
                    FocusPane::Target if state.target_idx > 0 => {
                        state.target_idx -= 1;
                    }
                    _ => {}
                },
                KeyCode::Down | KeyCode::Char('j') => match state.focus {
                    FocusPane::Source if state.source_idx + 1 < state.devices.len() => {
                        state.source_idx += 1;
                    }
                    FocusPane::Target if state.target_idx + 1 < state.devices.len() => {
                        state.target_idx += 1;
                    }
                    _ => {}
                },
                KeyCode::Char('b') => {
                    state.block_size = match state.block_size {
                        "64K" => "1M",
                        "1M" => "4M",
                        "4M" => "auto",
                        _ => "64K",
                    };
                }
                KeyCode::Char('e') => {
                    state.engine = match state.engine {
                        "auto" => "uring",
                        "uring" => "async",
                        "async" => "reflink",
                        "reflink" => "sync",
                        _ => "auto",
                    };
                }
                KeyCode::Char('h') => {
                    state.hash_algo = match state.hash_algo {
                        "none" => "blake3",
                        "blake3" => "blks",
                        "blks" => "sha256",
                        _ => "none",
                    };
                }
                KeyCode::Char('d') => {
                    state.direct_io = !state.direct_io;
                }
                KeyCode::Char('s') => {
                    state.swab = !state.swab;
                }
                KeyCode::Enter => {
                    if let Some(target) = state.devices.get(state.target_idx) {
                        if target.is_system_root {
                            state.status_msg = "FEHLER: Schreiben auf System-Root / gemountete Root-Partition ist blockiert!".to_string();
                        } else {
                            state.show_safety_modal = true;
                        }
                    }
                }
                _ => {}
            }
        }
    }

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    terminal.show_cursor()?;
    Ok(())
}

fn render_ui(f: &mut ratatui::Frame, state: &TuiState) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3), // Header
            Constraint::Min(12),   // Main Devices & Panes
            Constraint::Length(6), // Configuration Bar
            Constraint::Length(3), // Footer Status
        ])
        .split(f.area());

    // 1. Header
    let header = Paragraph::new(Line::from(vec![
        Span::styled(
            " blkcp-tui ",
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        ),
        Span::raw("| Next-Gen Linux Block Replication & Imaging Suite (GPLv3) | "),
        Span::styled(
            format!("Host Devices: {}", state.devices.len()),
            Style::default().fg(Color::Cyan),
        ),
    ]))
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" System Status "),
    );
    f.render_widget(header, chunks[0]);

    // 2. Main Center: Source & Target Panes
    let center_chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(50), Constraint::Percentage(50)])
        .split(chunks[1]);

    // Source Table
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
            }
            Row::new(vec![
                Cell::from(format!("{}{}", prefix, d.name)),
                Cell::from(d.size_human()),
                Cell::from(d.model.clone()),
            ])
            .style(style)
        })
        .collect();

    let source_table = Table::new(
        source_rows,
        [
            Constraint::Length(12),
            Constraint::Length(10),
            Constraint::Min(20),
        ],
    )
    .header(
        Row::new(vec!["Gerät", "Größe", "Modell"]).style(
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ),
    )
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" [Quelle / Input] (Tab zum Wechseln) ")
            .border_style(if state.focus == FocusPane::Source {
                Style::default().fg(Color::Yellow)
            } else {
                Style::default()
            }),
    );
    f.render_widget(source_table, center_chunks[0]);

    // Target Table
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
            }

            let mount_str = if d.is_system_root {
                "ROOT (BLOCKIERT)".to_string()
            } else if !d.mount_points.is_empty() {
                d.mount_points.join(", ")
            } else {
                "Nicht gemountet".to_string()
            };

            Row::new(vec![
                Cell::from(format!("{}{}", prefix, d.name)),
                Cell::from(d.size_human()),
                Cell::from(mount_str),
            ])
            .style(style)
        })
        .collect();

    let target_table = Table::new(
        target_rows,
        [
            Constraint::Length(12),
            Constraint::Length(10),
            Constraint::Min(20),
        ],
    )
    .header(
        Row::new(vec!["Zielgerät", "Größe", "Mount-Status"]).style(
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ),
    )
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" [Ziel / Output] (Rot = Systemschutz aktiv) ")
            .border_style(if state.focus == FocusPane::Target {
                Style::default().fg(Color::Yellow)
            } else {
                Style::default()
            }),
    );
    f.render_widget(target_table, center_chunks[1]);

    // 3. Options Bar
    let opt_lines = vec![
        Line::from(vec![
            Span::raw(" [B] Blockgröße: "),
            Span::styled(
                state.block_size,
                Style::default()
                    .fg(Color::Yellow)
                    .add_modifier(Modifier::BOLD),
            ),
            Span::raw("  |  [E] I/O-Engine: "),
            Span::styled(
                state.engine,
                Style::default()
                    .fg(Color::Cyan)
                    .add_modifier(Modifier::BOLD),
            ),
            Span::raw("  |  [H] In-Flight Hash: "),
            Span::styled(
                state.hash_algo,
                Style::default()
                    .fg(Color::Green)
                    .add_modifier(Modifier::BOLD),
            ),
        ]),
        Line::from(vec![
            Span::raw(" [D] Direct I/O (O_DIRECT): "),
            Span::styled(
                if state.direct_io { "AKTIV" } else { "AUS" },
                Style::default().fg(if state.direct_io {
                    Color::Green
                } else {
                    Color::DarkGray
                }),
            ),
            Span::raw("  |  [S] SIMD Byte-Swap: "),
            Span::styled(
                if state.swab { "AKTIV" } else { "AUS" },
                Style::default().fg(if state.swab {
                    Color::Green
                } else {
                    Color::DarkGray
                }),
            ),
            Span::raw("  |  [R] Geräte neu scannen"),
        ]),
    ];
    let opt_widget = Paragraph::new(opt_lines).block(
        Block::default()
            .borders(Borders::ALL)
            .title(" Transfer-Optionen & Hardware-Tuning "),
    );
    f.render_widget(opt_widget, chunks[2]);

    // 4. Footer Status
    let footer_text = format!(
        "Status: {} | [Enter]: Übertragung starten | [Q]: Beenden",
        state.status_msg
    );
    let footer = Paragraph::new(footer_text)
        .style(Style::default().fg(Color::LightCyan))
        .block(Block::default().borders(Borders::ALL).title(" Telemetrie "));
    f.render_widget(footer, chunks[3]);

    // Safety Modal Overlay
    if state.show_safety_modal {
        let area = centered_rect(65, 30, f.area());
        f.render_widget(Clear, area);

        let target_dev = &state.devices[state.target_idx];
        let modal_lines = vec![
            Line::from(Span::styled(
                "ACHTUNG: DESTRUKTIVER SCHREIBZUGRIFF!",
                Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
            )),
            Line::from(""),
            Line::from(format!(
                "Zielgerät: {} ({}, {})",
                target_dev.path.display(),
                target_dev.size_human(),
                target_dev.model
            )),
            Line::from("Alle bestehenden Daten und Partitionen auf diesem Datenträger"),
            Line::from("werden unwiderruflich überschrieben!"),
            Line::from(""),
            Line::from(vec![
                Span::raw("Wirklich fortfahren? "),
                Span::styled(
                    "[Y / Enter] JA, SCHREIBEN",
                    Style::default()
                        .fg(Color::Green)
                        .add_modifier(Modifier::BOLD),
                ),
                Span::raw("  |  "),
                Span::styled(
                    "[N / Esc] ABBRECHEN",
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                ),
            ]),
        ];

        let modal = Paragraph::new(modal_lines).block(
            Block::default()
                .borders(Borders::ALL)
                .title(" TARGET SAFETY GUARD ")
                .border_style(Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)),
        );
        f.render_widget(modal, area);
    }
}

fn centered_rect(percent_x: u16, percent_y: u16, r: Rect) -> Rect {
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
