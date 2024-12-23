use std::io::{self, Read};

const MAX_ACTIONS: usize = 2;
const MAX_FIELDS: usize = 4;
const MAX_ROWS: usize = 10;
const MAX_FIELD_LEN: usize = 254;

struct Parser {
    cnt: [usize; MAX_ACTIONS],
    nb: [[[usize; MAX_FIELDS]; MAX_ROWS]; MAX_ACTIONS],
    b: [[[[u8; MAX_FIELD_LEN]; MAX_FIELDS]; MAX_ROWS]; MAX_ACTIONS],
    i: usize,
    fidx: usize,
    fsz: usize,
}

impl Parser {
    fn new() -> Self {
        Parser {
            cnt: [0; MAX_ACTIONS],
            nb: [[[0; MAX_FIELDS]; MAX_ROWS]; MAX_ACTIONS],
            b: [[[[0; MAX_FIELD_LEN]; MAX_FIELDS]; MAX_ROWS]; MAX_ACTIONS],
            i: 0,
            fidx: 0,
            fsz: 0,
        }
    }

    fn consume(&mut self, c: u8) -> Result<bool, String> {
        if c == b',' || c == b'\n' {
            if self.fidx > 0 {
                self.nb[self.i][self.cnt[self.i]][self.fidx - 1] = self.fsz;
            }

            self.fidx += 1;
            self.fsz = 0;

            if self.fidx == 5 {
                self.cnt[self.i] += 1;
                self.fidx = 0;
            }

            if c == b'\n' {
                return Ok(true);
            }
        } else {
            if self.fidx == 0 {
                match c {
                    b'1' => self.i = 0,
                    b'2' => self.i = 1,
                    _ => return Err(format!("unknown identifier '{}'", c as char)),
                }
            } else {
                self.b[self.i][self.cnt[self.i]][self.fidx - 1][self.fsz] = c;
                self.fsz += 1;
            }
        }
        Ok(false)
    }

    fn print_batch_counts(&mut self) {
        for i in 0..MAX_ACTIONS {
            if self.cnt[i] > 0 {
                println!(
                    "Batch {} has {} items.",
                    i + 1,
                    self.cnt[i]
                );
                self.cnt[i] = 0; // Reset count after printing
            }
        }
    }
}

fn main() {
    let mut parser = Parser::new();
    let stdin = io::stdin();
    let mut handle = stdin.lock();
    let mut buffer = [0; 8192];

    while let Ok(n) = handle.read(&mut buffer) {
        if n == 0 {
            break;
        }

        for &byte in &buffer[..n] {
            if let Ok(ready) = parser.consume(byte) {
                if ready {
                    parser.print_batch_counts();
                }
            } else {
                eprintln!("Failed to parse input");
                return;
            }
        }
    }
}
