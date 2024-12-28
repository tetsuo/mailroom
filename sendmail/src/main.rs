use aws_config::meta::region::RegionProviderChain;
use aws_sdk_ses::types::{BulkEmailDestination, Destination};
use aws_sdk_ses::{Client, Error};
use chrono::Utc;
use std::env;
use std::fs;
use std::fs::File;
use std::io::Write;
use std::io::{self, Read};
use std::path::Path;
use std::process;
use std::time::Instant;

const MAX_ACTIONS: usize = 2;
const MAX_FIELDS: usize = 4;
const MAX_ROWS: usize = 10;
const MAX_FIELD_LEN: usize = 254;

macro_rules! log {
    ($($arg:tt)*) => {{
        let timestamp = Utc::now().format("%Y/%m/%d %H:%M:%S");
        eprintln!("{} [SES] {}", timestamp, format_args!($($arg)*));
    }};
}

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

    async fn finalize(
        &mut self,
        client: &Client,
        config_set_name: &str,
        from_email: &str,
        outdir: &str,
        dev_mode: bool,
    ) {
        for i in 0..MAX_ACTIONS {
            let mut destinations = Vec::new();

            for j in 0..self.cnt[i] {
                let b = &self.b[i][j];
                let nb = &self.nb[i][j];

                let to_address = String::from_utf8_lossy(&b[0][..nb[0]]).to_string();
                let destination = Destination::builder().to_addresses(to_address).build();

                let template_data = if i == 0 {
                    format!(
                        "{{\"login\":\"{}\",\"secret\":\"{}\"}}",
                        String::from_utf8_lossy(&b[1][..nb[1]]),
                        String::from_utf8_lossy(&b[2][..nb[2]])
                    )
                } else {
                    format!(
                        "{{\"login\":\"{}\",\"secret\":\"{}\",\"code\":\"{}\"}}",
                        String::from_utf8_lossy(&b[1][..nb[1]]),
                        String::from_utf8_lossy(&b[2][..nb[2]]),
                        String::from_utf8_lossy(&b[3][..nb[3]])
                    )
                };

                let bulk_dest = BulkEmailDestination::builder()
                    .destination(destination)
                    .replacement_template_data(template_data)
                    .build();

                destinations.push(bulk_dest);
            }

            self.cnt[i] = 0;

            if destinations.is_empty() {
                continue;
            }

            let template_name = match i {
                0 => "activationv1",
                1 => "passwordrecoveryv1",
                _ => unreachable!(),
            };

            let default_template_data = match i {
                0 => r#"{"login":"","secret":""}"#,
                1 => r#"{"login":"","secret":"","code":""}"#,
                _ => unreachable!(),
            };

            if dev_mode {
                println!("Sending bulk email 🚀");
                println!("  Template Name         = {}", template_name);
                println!("  Configuration Set     = {}", config_set_name);
                println!("  From                  = {}", from_email);
                println!("  Default Template Data = {}", default_template_data);
                println!("  Destinations ({})", destinations.len());
                for (idx, dest) in destinations.iter().enumerate() {
                    println!("    {}. {:?}", idx + 1, dest);
                }
                println!();

                continue;
            }

            let mut email_builder = client
                .send_bulk_templated_email()
                .template(template_name)
                .configuration_set_name(config_set_name)
                .source(from_email)
                .default_template_data(default_template_data);

            for destination in &destinations {
                email_builder = email_builder.destinations(destination.clone());
            }

            let start_time = Instant::now();

            match email_builder.send().await {
                Ok(output) => {
                    println!("SendBulkTemplatedEmailResponse:\n{:#?}", output);
                    for (idx, status) in output.status().iter().enumerate() {
                        let code = status.status().map(|s| s.as_str()).unwrap_or("UNKNOWN");
                        println!("  Destination #{} => Status: {}", idx, code);
                    }
                }
                Err(aws_sdk_ses::error::SdkError::ServiceError(err)) => {
                    // Extract and write the raw HTTP response to a file
                    let file_name = format!(
                        "ses_{}_{}.http",
                        Utc::now().format("%Y%m%d%H%M%S%.3f").to_string(),
                        i
                    );

                    let full_path = Path::new(outdir).join(file_name);

                    match File::create(&full_path) {
                        Ok(mut file) => {
                            let result = (|| -> Result<usize, std::io::Error> {
                                let mut total_bytes_written = 0;

                                let status_line = format!("HTTP/1.1 {}\n", err.raw().status());
                                total_bytes_written += file.write(status_line.as_bytes())?;

                                for (key, value) in err.raw().headers().iter() {
                                    let header = format!("{}: {}\n", key, value);
                                    total_bytes_written += file.write(header.as_bytes())?;
                                }

                                total_bytes_written += file.write(b"\n")?;

                                if let Some(bytes) = err.raw().body().bytes() {
                                    let raw_body = String::from_utf8_lossy(bytes);
                                    total_bytes_written += file.write(raw_body.as_bytes())?;
                                } else {
                                    let no_body_message = "Empty body.\n";
                                    total_bytes_written +=
                                        file.write(no_body_message.as_bytes())?;
                                }

                                Ok(total_bytes_written)
                            })();

                            let duration = start_time.elapsed();

                            match result {
                                Ok(total_bytes_written) => {
                                    log!(
                                        "{} bytes written to {} ({:.2} seconds)",
                                        total_bytes_written,
                                        full_path.display(),
                                        duration.as_secs_f64()
                                    );
                                }
                                Err(e) => {
                                    log!(
                                        "ERROR: failed to write to file {}: {}",
                                        full_path.display(),
                                        e
                                    );
                                }
                            }
                        }
                        Err(e) => {
                            log!(
                                "ERROR: failed to create file {}: {}",
                                full_path.display(),
                                e
                            );
                        }
                    }
                }
                Err(aws_sdk_ses::error::SdkError::TimeoutError { .. }) => {
                    log!("ERROR: connection timeout out");
                }
                Err(aws_sdk_ses::error::SdkError::DispatchFailure(err)) => {
                    log!("ERROR: dispatch failure; {:#?}", err);
                }
                Err(err) => {
                    log!("ERROR: unexpected error; {:#?}", err);
                }
            }
        }
    }
}

#[tokio::main]
async fn main() -> Result<(), Error> {
    let dev_mode = env::var("MF_DEBUG").unwrap_or_else(|_| "false".to_string()) == "true";
    let config_set_name = env::var("MF_SES_CONFIG_SET").unwrap_or_else(|_| "default".to_string());
    let from_email = env::var("MF_SES_SOURCE").unwrap_or_else(|_| "noreply@localhost".to_string());
    let outdir = env::var("MF_SES_OUTPUT_PATH").unwrap_or_else(|_| "./output".to_string());

    log!(
        "configured; debug={} config_set={} source={} output_path={}",
        dev_mode,
        config_set_name,
        from_email,
        outdir,
    );

    if let Err(e) = fs::create_dir_all(&outdir) {
        log!("ERROR: failed to create output directory {}: {}", outdir, e);
        process::exit(1);
    }

    let region_provider = RegionProviderChain::default_provider().or_else("us-east-1");
    let config = aws_config::from_env().region(region_provider).load().await;
    let client = Client::new(&config);

    let mut parser = Parser::new();
    let stdin = io::stdin();
    let mut handle = stdin.lock();
    let mut buffer = [0; 8192];

    loop {
        match handle.read(&mut buffer) {
            Ok(0) => {
                log!("ERROR: end of input stream");
                process::exit(1);
            }
            Ok(n) => {
                for &byte in &buffer[..n] {
                    if let Ok(ready) = parser.consume(byte) {
                        if ready {
                            parser
                                .finalize(&client, &config_set_name, &from_email, &outdir, dev_mode)
                                .await;
                        }
                    } else {
                        log!("ERROR: failed to parse input");
                        process::exit(1);
                    }
                }
            }
            Err(e) => {
                log!("ERROR: failed to read from stdin: {}", e);
                process::exit(1);
            }
        }
    }
}
