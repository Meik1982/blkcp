use blkcp::tui_device::list_directory_entries;
use blkcp::tui_render::render_ui;
use blkcp::tui_state::{FocusPane, ModalDialog, TuiState};

use crossterm::{
    event::{self, Event, KeyCode, KeyModifiers},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{backend::CrosstermBackend, Terminal};
use std::io;
use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::time::Duration;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let mut state = TuiState::new();

    while state.running {
        state.poll_active_job();

        terminal.draw(|f| {
            render_ui(f, &state);
        })?;

        if event::poll(Duration::from_millis(50))? {
            if let Event::Key(key) = event::read()? {
                handle_key_event(&mut state, key.code, key.modifiers);
            }
        }
    }

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    terminal.show_cursor()?;
    Ok(())
}

fn handle_key_event(state: &mut TuiState, code: KeyCode, _modifiers: KeyModifiers) {
    // 1. If a transfer job is actively running, allow immediate cancellation via X or Esc
    if state.active_job.is_some() {
        if matches!(code, KeyCode::Char('x') | KeyCode::Char('X') | KeyCode::Esc) {
            if let Some(ref job) = state.active_job {
                job.cancel_flag.store(true, Ordering::Relaxed);
                state.status_msg = "Abbruchsignal gesendet...".to_string();
            }
        }
        return;
    }

    // 2. Modal Dialog Handling
    match &mut state.modal {
        ModalDialog::None => {}
        ModalDialog::SafetyConfirm => {
            match code {
                KeyCode::Char('y') | KeyCode::Char('Y') | KeyCode::Enter => {
                    state.modal = ModalDialog::None;
                    state.start_transfer();
                }
                KeyCode::Char('n') | KeyCode::Char('N') | KeyCode::Esc => {
                    state.modal = ModalDialog::None;
                    state.status_msg = "Schreibvorgang vom Benutzer abgebrochen.".to_string();
                }
                _ => {}
            }
            return;
        }
        ModalDialog::PathInput { for_target, buffer } => {
            match code {
                KeyCode::Esc => {
                    state.modal = ModalDialog::None;
                }
                KeyCode::Enter => {
                    let trimmed = buffer.trim().to_string();
                    let is_tgt = *for_target;
                    state.modal = ModalDialog::None;
                    if !trimmed.is_empty() {
                        state.add_or_select_custom_path(PathBuf::from(trimmed), is_tgt);
                    }
                }
                KeyCode::Backspace => {
                    buffer.pop();
                }
                KeyCode::Char(c) => {
                    buffer.push(c);
                }
                _ => {}
            }
            return;
        }
        ModalDialog::LimitInput {
            field,
            limit_buf,
            skip_buf,
            seek_buf,
        } => {
            match code {
                KeyCode::Esc => {
                    state.modal = ModalDialog::None;
                }
                KeyCode::Tab | KeyCode::Down => {
                    *field = (*field + 1) % 3;
                }
                KeyCode::BackTab | KeyCode::Up => {
                    *field = (*field + 2) % 3;
                }
                KeyCode::Enter => {
                    state.limit_str = limit_buf.trim().to_string();
                    state.skip_str = skip_buf.trim().to_string();
                    state.seek_str = seek_buf.trim().to_string();
                    state.modal = ModalDialog::None;
                    state.status_msg = format!(
                        "Limits aktualisiert: Limit='{}', Skip='{}', Seek='{}'",
                        state.limit_str, state.skip_str, state.seek_str
                    );
                }
                KeyCode::Backspace => match *field {
                    0 => {
                        limit_buf.pop();
                    }
                    1 => {
                        skip_buf.pop();
                    }
                    _ => {
                        seek_buf.pop();
                    }
                },
                KeyCode::Char(c) => match *field {
                    0 => limit_buf.push(c),
                    1 => skip_buf.push(c),
                    _ => seek_buf.push(c),
                },
                _ => {}
            }
            return;
        }
        ModalDialog::FileBrowser {
            for_target,
            current_dir,
            entries,
            selected_idx,
        } => {
            match code {
                KeyCode::Esc | KeyCode::Char('q') => {
                    state.modal = ModalDialog::None;
                }
                KeyCode::Up | KeyCode::Char('k') => {
                    if *selected_idx > 0 {
                        *selected_idx -= 1;
                    }
                }
                KeyCode::Down | KeyCode::Char('j') => {
                    if *selected_idx + 1 < entries.len() {
                        *selected_idx += 1;
                    }
                }
                KeyCode::Enter => {
                    if let Some(entry) = entries.get(*selected_idx).cloned() {
                        if entry.is_dir {
                            *current_dir = entry.path;
                            *entries = list_directory_entries(current_dir);
                            *selected_idx = 0;
                        } else {
                            let is_tgt = *for_target;
                            let chosen = entry.path;
                            state.modal = ModalDialog::None;
                            state.add_or_select_custom_path(chosen, is_tgt);
                        }
                    }
                }
                _ => {}
            }
            return;
        }
    }

    // 3. Normal Navigation & Module Shortcuts
    match code {
        KeyCode::Char('q') | KeyCode::Esc => state.running = false,
        KeyCode::Tab => {
            state.focus = match state.focus {
                FocusPane::Source => FocusPane::Target,
                FocusPane::Target => FocusPane::Modules,
                FocusPane::Modules => FocusPane::Source,
            };
        }
        KeyCode::BackTab => {
            state.focus = match state.focus {
                FocusPane::Source => FocusPane::Modules,
                FocusPane::Target => FocusPane::Source,
                FocusPane::Modules => FocusPane::Target,
            };
        }
        KeyCode::Up | KeyCode::Char('k') => match state.focus {
            FocusPane::Source if state.source_idx > 0 => state.source_idx -= 1,
            FocusPane::Target if state.target_idx > 0 => state.target_idx -= 1,
            FocusPane::Modules if state.module_idx > 0 => state.module_idx -= 1,
            _ => {}
        },
        KeyCode::Down => match state.focus {
            FocusPane::Source if state.source_idx + 1 < state.devices.len() => {
                state.source_idx += 1
            }
            FocusPane::Target if state.target_idx + 1 < state.devices.len() => {
                state.target_idx += 1
            }
            FocusPane::Modules if state.module_idx + 1 < 16 => state.module_idx += 1,
            _ => {}
        },
        KeyCode::Char(' ') => {
            if state.focus == FocusPane::Modules {
                state.toggle_selected_module();
            }
        }
        // Global Hotkeys for all modules & combinations
        KeyCode::Char('b') | KeyCode::Char('B') => state.cycle_block_size(),
        KeyCode::Char('e') | KeyCode::Char('E') => state.cycle_engine(),
        KeyCode::Char('h') | KeyCode::Char('H') => state.cycle_hash(),
        KeyCode::Char('j') | KeyCode::Char('J') => state.cycle_threads(),
        KeyCode::Char('u') | KeyCode::Char('U') => state.cycle_queue_depth(),
        KeyCode::Char('d') | KeyCode::Char('D') => state.direct_io = !state.direct_io,
        KeyCode::Char('c') | KeyCode::Char('C') => state.nocache = !state.nocache,
        KeyCode::Char('a') | KeyCode::Char('A') => state.sparse = !state.sparse,
        KeyCode::Char('w') | KeyCode::Char('W') => state.swab = !state.swab,
        KeyCode::Char('y') | KeyCode::Char('Y') => state.sync_pad = !state.sync_pad,
        KeyCode::Char('t') | KeyCode::Char('T') => state.notrunc = !state.notrunc,
        KeyCode::Char('g') | KeyCode::Char('G') => state.sync_mode = state.sync_mode.next(),
        KeyCode::Char('m') | KeyCode::Char('M') => state.noerror = !state.noerror,
        KeyCode::Char('n') | KeyCode::Char('N') => state.dry_run = !state.dry_run,
        KeyCode::Char('p') | KeyCode::Char('P') => {
            state.include_partitions = !state.include_partitions;
            state.refresh_devices();
        }
        KeyCode::Char('r') | KeyCode::Char('R') => state.refresh_devices(),
        KeyCode::Char('l') | KeyCode::Char('L') => {
            state.modal = ModalDialog::LimitInput {
                field: 0,
                limit_buf: state.limit_str.clone(),
                skip_buf: state.skip_str.clone(),
                seek_buf: state.seek_str.clone(),
            };
        }
        KeyCode::Char('i') | KeyCode::Char('I') => {
            let cur = state
                .devices
                .get(state.source_idx)
                .map(|d| d.path.to_string_lossy().to_string())
                .unwrap_or_default();
            state.modal = ModalDialog::PathInput {
                for_target: false,
                buffer: cur,
            };
        }
        KeyCode::Char('o') | KeyCode::Char('O') => {
            let cur = state
                .devices
                .get(state.target_idx)
                .map(|d| d.path.to_string_lossy().to_string())
                .unwrap_or_default();
            state.modal = ModalDialog::PathInput {
                for_target: true,
                buffer: cur,
            };
        }
        KeyCode::Char('f') | KeyCode::Char('F') => {
            let for_target = state.focus == FocusPane::Target;
            let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
            let entries = list_directory_entries(&cwd);
            state.modal = ModalDialog::FileBrowser {
                for_target,
                current_dir: cwd,
                entries,
                selected_idx: 0,
            };
        }
        // Workflow Combination Presets 1..8 & Auto-Resolve
        KeyCode::Char('1') => state.apply_preset(1),
        KeyCode::Char('2') => state.apply_preset(2),
        KeyCode::Char('3') => state.apply_preset(3),
        KeyCode::Char('4') => state.apply_preset(4),
        KeyCode::Char('5') => state.apply_preset(5),
        KeyCode::Char('6') => state.apply_preset(6),
        KeyCode::Char('7') => state.apply_preset(7),
        KeyCode::Char('8') => state.apply_preset(8),
        KeyCode::Char('z') | KeyCode::Char('Z') => state.auto_resolve_combinations(),
        KeyCode::Enter => {
            if state.dry_run {
                state.start_transfer();
                return;
            }
            if let Some(target) = state.devices.get(state.target_idx) {
                if target.is_system_root && !state.force_override {
                    state.status_msg =
                        "BLOCKIERT: Ziel ist ein gemountetes System-Laufwerk! (Sicherheits-Sperre aktiv)"
                            .to_string();
                } else if target.path == std::path::Path::new("/dev/null") {
                    // Writing to /dev/null is non-destructive -> start immediately
                    state.start_transfer();
                } else {
                    state.modal = ModalDialog::SafetyConfirm;
                }
            }
        }
        _ => {}
    }
}
