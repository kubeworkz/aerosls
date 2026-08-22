#[test]
fn smoke_map_type() {
    let src = r#"
        @version("aeroidl/1.0")
        @namespace("test")
        interface Svc {
            b(x: map<string, u32>) -> string;
        }
    "#;
    let result = aeroidl_cc::parser::parse(src);
    match result {
        Ok(doc) => println!("OK: {doc:?}"),
        Err(e) => panic!("parse error: {e:?}"),
    }
}

#[test]
fn smoke_option_result() {
    let src = r#"
        @version("aeroidl/1.0")
        @namespace("test")
        interface Svc {
            a(x: Option<i32>) -> Result<(), string>;
        }
    "#;
    let result = aeroidl_cc::parser::parse(src);
    match result {
        Ok(doc) => println!("OK: {doc:?}"),
        Err(e) => panic!("parse error: {e:?}"),
    }
}

#[test]
fn smoke_two_methods() {
    let src = r#"
        @version("aeroidl/1.0")
        @namespace("test")
        interface Svc {
            a(x: Option<i32>) -> Result<(), string>;
            b(x: map<string, u32>) -> string;
        }
    "#;
    let result = aeroidl_cc::parser::parse(src);
    match result {
        Ok(doc) => println!("OK: {doc:?}"),
        Err(e) => panic!("parse error: {e:?}"),
    }
}
