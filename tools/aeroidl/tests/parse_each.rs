fn test_file(name: &str, src: &str) {
    let start = std::time::Instant::now();
    let result = aeroidl_cc::parser::parse(src);
    let elapsed = start.elapsed();
    match result {
        Ok(doc) => println!("OK  [{name}] {elapsed:?} — {n} decls", n = doc.declarations.len()),
        Err(e) => println!("ERR [{name}] {elapsed:?} — {e:?}"),
    }
}

#[test]
fn parse_logging() {
    test_file("logging", include_str!("../../../idl/logging.aeroidl"));
}

#[test]
fn parse_calculator() {
    test_file("calculator", include_str!("../../../idl/calculator.aeroidl"));
}

#[test]
fn parse_imageproc() {
    test_file("imageproc", include_str!("../../../idl/imageproc.aeroidl"));
}
