#!/usr/bin/env php
<?php

// Parse command line arguments
$options = getopt('h', [
    'host:',
    'port:',
    'user:',
    'password::',
    'db:',
    'table:',
    'help'
]);

// Default values
$dbName = $options['db'] ?? 'test';
$dbHost = $options['host'] ?? '127.0.0.1';
$dbPort = $options['port'] ?? 3306;
$dbUser = $options['user'] ?? 'root';
$dbPass = $options['password'] ?? '';
$dbTable = $options['table'] ?? '';

// Show help if requested
if (isset($options['h']) || isset($options['help'])) {
    usage();
}

// Connect to database
try {
    $dsn = "mysql:host=$dbHost;port=$dbPort;dbname=$dbName;charset=utf8mb4";
    $pdo = new PDO($dsn, $dbUser, $dbPass);
    $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
} catch (PDOException $e) {
    usage("Can't connect to mysql: " . $e->getMessage());
}

// Start output
print("#ifndef table_defs_h\n#define table_defs_h\n\n");
print("// Table definitions\ntable_def_t table_definitions[] = {\n");

// Get tables
$stmt = $pdo->prepare("SHOW TABLES");
$stmt->execute();

while ($row = $stmt->fetch(PDO::FETCH_NUM)) {
    $table = $row[0];
    
    // Skip if not a requested specific table
    if ($dbTable !== '' && $table !== $dbTable) {
        continue;
    }
    
    // Skip if it is not an innodb table
    if (!preg_match('/innodb/i', getTableStorageEngine($pdo, $table))) {
        continue;
    }
    
    // Get fields list for table
    $stmt2 = $pdo->prepare("SHOW FIELDS FROM $table");
    $stmt2->execute();
    $fields = $stmt2->fetchAll(PDO::FETCH_ASSOC);
    
    // Get primary key fields
    $stmt2 = $pdo->prepare("SHOW INDEXES FROM $table");
    $stmt2->execute();
    $pkFields = [];
    while ($field = $stmt2->fetch(PDO::FETCH_ASSOC)) {
        if ($field['Key_name'] === 'PRIMARY') {
            $pkFields[] = $field;
        }
    }
    
    // If no primary keys defined, check unique keys and use first one as primary
    if (empty($pkFields)) {
        $stmt2 = $pdo->prepare("SHOW INDEXES FROM $table");
        $stmt2->execute();
        $pkName = null;
        while ($field = $stmt2->fetch(PDO::FETCH_ASSOC)) {
            if ($field['Non_unique'] == 0) {
                if ($pkName === null) {
                    $pkName = $field['Key_name'];
                }
                if ($field['Key_name'] === $pkName) {
                    $pkFields[] = $field;
                }
            }
        }
    }
    
    dumpTableDef($pdo, $table, $fields, $pkFields);
}

print("};\n\n#endif\n");

function dumpTableDef($pdo, $table, $fields, $pkFields) {
    printf("\t{\n\t\tname: \"%s\",\n\t\t{\n", $table);
    
    // Dump all PK fields
    foreach ($pkFields as $pkField) {
        foreach ($fields as $field) {
            if ($field['Field'] === $pkField['Column_name']) {
                dumpField($pdo, $table, $field);
                break;
            }
        }
    }
    
    // Dump system PK if no PK fields found
    if (empty($pkFields)) {
        dumpFieldLow([
            'Name' => 'DB_ROW_ID',
            'ParsedType' => 'FT_INTERNAL',
            'FixedLen' => 6,
            'Null' => false
        ]);
    }
    
    // Dump 2 more sys fields
    dumpFieldLow([
        'Name' => 'DB_TRX_ID',
        'ParsedType' => 'FT_INTERNAL',
        'FixedLen' => 6,
        'Null' => false
    ]);
    
    dumpFieldLow([
        'Name' => 'DB_ROLL_PTR',
        'ParsedType' => 'FT_INTERNAL',
        'FixedLen' => 7,
        'Null' => false
    ]);
    
    // Dump the rest of the fields
    foreach ($fields as $field) {
        $isPK = false;
        foreach ($pkFields as $pkField) {
            if ($field['Field'] === $pkField['Column_name']) {
                $isPK = true;
                break;
            }
        }
        if (!$isPK) {
            dumpField($pdo, $table, $field);
        }
    }

    printf("\t\t\t{ type: FT_NONE }\n");
    printf("\t\t}\n\t},\n");
}

function getUIntLimits($len) {
    switch ($len) {
        case 1: return [0, 255];
        case 2: return [0, 65535];
        case 3: return [0, "16777215UL"];
        case 4: return [0, "4294967295ULL"];
        case 8: return [0, "18446744073709551615ULL"];
        default: return [0, 30000];
    }
}

function getIntLimits($len) {
    switch ($len) {
        case 1: return [-128, 127];
        case 2: return [-32768, 32767];
        case 3: return ["-8388608L", "8388607L"];
        case 4: return ["-2147483648LL", "2147483647LL"];
        case 8: return ["-9223372036854775806LL", "9223372036854775807LL"];
        default: return [0, 30000];
    }
}

function dumpFieldLow($info) {
    printf("\t\t\t{ /* %s */\n", $info['Type'] ?? '');
    printf("\t\t\t\tname: \"%s\",\n", $info['Name']);
    printf("\t\t\t\ttype: %s,\n", $info['ParsedType']);

    if (isset($info['FixedLen'])) {
        printf("\t\t\t\tfixed_length: %d,\n", $info['FixedLen']);
    } else {
        printf("\t\t\t\tmin_length: %d,\n", $info['MinLen']);
        printf("\t\t\t\tmax_length: %d,\n", $info['MaxLen']);
    }
    
    printf("\n");
    
    if ($info['ParsedType'] === 'FT_TEXT' || $info['ParsedType'] === 'FT_CHAR') {
        printf("\t\t\t\thas_limits: FALSE,\n");
        printf("\t\t\t\tlimits: {\n");
        printf("\t\t\t\t\tcan_be_null: %s,\n", $info['Null'] ? 'TRUE' : 'FALSE');
        printf("\t\t\t\t\tchar_min_len: 0,\n");
        printf("\t\t\t\t\tchar_max_len: %d,\n", $info['MaxLen'] + ($info['FixedLen'] ?? 0));
        printf("\t\t\t\t\tchar_ascii_only: TRUE\n");
        printf("\t\t\t\t},\n\n");
    }

    if ($info['ParsedType'] === 'FT_DECIMAL') {
        printf("\t\t\t\tdecimal_precision: %d,\n", $info['decimal_precision']);
        printf("\t\t\t\tdecimal_digits: %d,\n", $info['decimal_digits']);
    }

    if ($info['ParsedType'] === 'FT_INT') {
        list($min, $max) = getIntLimits($info['FixedLen']);
        printf("\t\t\t\thas_limits: FALSE,\n");
        printf("\t\t\t\tlimits: {\n");
        printf("\t\t\t\t\tcan_be_null: %s,\n", $info['Null'] ? 'TRUE' : 'FALSE');
        printf("\t\t\t\t\tint_min_val: %s,\n", $min);
        printf("\t\t\t\t\tint_max_val: %s\n", $max);
        printf("\t\t\t\t},\n\n");
    }

    if ($info['ParsedType'] === 'FT_UINT') {
        list($min, $max) = getUIntLimits($info['FixedLen']);
        printf("\t\t\t\thas_limits: FALSE,\n");
        printf("\t\t\t\tlimits: {\n");
        printf("\t\t\t\t\tcan_be_null: %s,\n", $info['Null'] ? 'TRUE' : 'FALSE');
        printf("\t\t\t\t\tuint_min_val: %s,\n", $min);
        printf("\t\t\t\t\tuint_max_val: %s\n", $max);
        printf("\t\t\t\t},\n\n");
    }

    if ($info['ParsedType'] === 'FT_ENUM') {
        printf("\t\t\t\thas_limits: FALSE,\n");
        printf("\t\t\t\tlimits: {\n");
        printf("\t\t\t\t\tcan_be_null: %s,\n", $info['Null'] ? 'TRUE' : 'FALSE');
        printf("\t\t\t\t\tenum_values_count: %d,\n", count($info['Values']));
        printf("\t\t\t\t\tenum_values: { \"%s\" }\n", implode('", "', $info['Values']));
        printf("\t\t\t\t},\n\n");
    }

    printf("\t\t\t\tcan_be_null: %s\n", $info['Null'] ? 'TRUE' : 'FALSE');
    printf("\t\t\t},\n");
}


function dumpField($pdo, $table, $field) {
    $info = [
        'Null' => $field['Null'] === 'YES',
        'Name' => $field['Field'],
        'Type' => $field['Type']
    ];
    
    $typeInfo = parseFieldType($field['Type']);
    if ($typeInfo['type'] === 'FT_INT' && isFieldUnsigned($pdo, $table, $field['Field'])) {
        $typeInfo['type'] = 'FT_UINT';
    }

    if ($typeInfo['type'] === 'FT_CHAR') {
        $maxlen = getMaxlen($pdo, $table, $field['Field']);
        if ($maxlen > 1) {
            if (isset($typeInfo['fixed_len'])) {
                // If type is CHAR(x)
                $typeInfo['min_len'] = $typeInfo['fixed_len'];
                $typeInfo['max_len'] = $typeInfo['fixed_len'] * $maxlen;
                unset($typeInfo['fixed_len']);
            } else {
                $typeInfo['max_len'] *= $maxlen;
            }
        }
    }
    
    $info['Values'] = $typeInfo['values'] ?? null;
    $info['ParsedType'] = $typeInfo['type'];
    $info['MinLen'] = $typeInfo['min_len'] ?? null;
    $info['MaxLen'] = $typeInfo['max_len'] ?? null;
    $info['FixedLen'] = $typeInfo['fixed_len'] ?? null;
    $info['decimal_precision'] = $typeInfo['decimal_precision'] ?? null;
    $info['decimal_digits'] = $typeInfo['decimal_digits'] ?? null;
    dumpFieldLow($info);
}

function isFieldUnsigned($pdo, $table, $field) {
    $stmt = $pdo->prepare("SHOW CREATE TABLE $table");
    $stmt->execute();
    $row = $stmt->fetch(PDO::FETCH_NUM);
    return (bool)preg_match("/{$field}[^,]*unsigned/i", $row[1]);
}


function getMaxlen($pdo, $table, $field) {
    $stmt = $pdo->prepare("SHOW FULL COLUMNS FROM `$table` LIKE ?");
    $stmt->execute([$field]);
    $row = $stmt->fetch(PDO::FETCH_NUM);
    $collation = $row[2];
    
    $stmt = $pdo->prepare("SHOW COLLATION LIKE ?");
    $stmt->execute([$collation]);
    $row = $stmt->fetch(PDO::FETCH_NUM);
    $charset = $row[1];
    
    $stmt = $pdo->prepare("SHOW CHARSET LIKE ?");
    $stmt->execute([$charset]);
    $row = $stmt->fetch(PDO::FETCH_NUM);
    return (int)$row[3];
}

function getTableStorageEngine($pdo, $table) {
    $stmt = $pdo->prepare("SHOW TABLE STATUS LIKE ?");
    $stmt->execute([$table]);
    $row = $stmt->fetch(PDO::FETCH_ASSOC);
    return $row['Engine'];
}

// Helper function for usage information
function usage($msg = '') {
    if ($msg) {
        echo "Error: $msg\n";
    }
    
    echo "Usage: " . $_SERVER['argv'][0] . " [options]\n" .
         "Where options are:\n" .
         "\t--host     - mysql host\n" .
         "\t--port     - mysql port\n" .
         "\t--user     - mysql username\n" .
         "\t--password - mysql password\n" .
         "\t--db       - mysql database\n" .
         "\t--table    - specific table only\n" .
         "\t--help     - show this help\n\n";
    exit(1);
}

function parseFieldType($type) {
    if (preg_match('/DATETIME/i', $type)) {
        return ['type' => 'FT_DATETIME', 'fixed_len' => 8];
    }
    
    if (preg_match('/TIMESTAMP/i', $type)) {
        return ['type' => 'FT_TIMESTAMP', 'fixed_len' => 4];
    }
    
    if (preg_match('/DATE/i', $type)) {
        return ['type' => 'FT_DATE', 'fixed_len' => 3];
    }
    
    if (preg_match('/TIME/i', $type)) {
        return ['type' => 'FT_TIME', 'fixed_len' => 3];
    }
    
    if (preg_match('/YEAR/i', $type)) {
        return ['type' => 'FT_UINT', 'fixed_len' => 1];
    }
    
    if (preg_match('/^TINYINT/i', $type)) {
        return ['type' => 'FT_INT', 'fixed_len' => 1];
    }
    
    if (preg_match('/^SMALLINT/i', $type)) {
        return ['type' => 'FT_INT', 'fixed_len' => 2];
    }
    
    if (preg_match('/^MEDIUMINT/i', $type)) {
        return ['type' => 'FT_INT', 'fixed_len' => 3];
    }
    
    if (preg_match('/^INT/i', $type)) {
        return ['type' => 'FT_INT', 'fixed_len' => 4];
    }
    
    if (preg_match('/^BIGINT/i', $type)) {
        return ['type' => 'FT_INT', 'fixed_len' => 8];
    }
    
    if (preg_match('/^CHAR\((\d+)\)/i', $type, $matches)) {
        return ['type' => 'FT_CHAR', 'fixed_len' => (int)$matches[1]];
    }
    
    if (preg_match('/VARCHAR\((\d+)\)/i', $type, $matches)) {
        return ['type' => 'FT_CHAR', 'min_len' => 0, 'max_len' => (int)$matches[1]];
    }
    
    if (preg_match('/^TINYTEXT$/i', $type)) {
        return ['type' => 'FT_TEXT', 'min_len' => 0, 'max_len' => 255];
    }
    
    if (preg_match('/^TEXT$/i', $type)) {
        return ['type' => 'FT_TEXT', 'min_len' => 0, 'max_len' => 65535];
    }
    
    if (preg_match('/^MEDIUMTEXT$/i', $type)) {
        return ['type' => 'FT_TEXT', 'min_len' => 0, 'max_len' => 16777215];
    }
    
    if (preg_match('/^LONGTEXT$/i', $type)) {
        return ['type' => 'FT_TEXT', 'min_len' => 0, 'max_len' => 4294967295];
    }
    
    if (preg_match('/^TINYBLOB$/i', $type)) {
        return ['type' => 'FT_BLOB', 'min_len' => 0, 'max_len' => 255];
    }
    
    if (preg_match('/^BLOB$/i', $type)) {
        return ['type' => 'FT_BLOB', 'min_len' => 0, 'max_len' => 65535];
    }
    
    if (preg_match('/^MEDIUMBLOB$/i', $type)) {
        return ['type' => 'FT_BLOB', 'min_len' => 0, 'max_len' => 16777215];
    }
    
    if (preg_match('/^LONGBLOB$/i', $type)) {
        return ['type' => 'FT_BLOB', 'min_len' => 0, 'max_len' => 4294967295];
    }
    
    if (preg_match('/^FLOAT/i', $type)) {
        return ['type' => 'FT_FLOAT', 'fixed_len' => 4];
    }
    
    if (preg_match('/^DOUBLE/i', $type)) {
        return ['type' => 'FT_DOUBLE', 'fixed_len' => 8];
    }
    
    if (preg_match('/^ENUM\(\'(.*)\'\)/i', $type, $matches)) {
        $enumValues = array_map('trim', explode("','", $matches[1]));
        return ['type' => 'FT_ENUM', 'fixed_len' => 1, 'values' => $enumValues];
    }
    
    if (preg_match('/^DECIMAL\((\d+),\s*(\d+)\)/i', $type, $matches)) {
        $lenBytes = ceil(($matches[1] - $matches[2]) * 4 / 9) + ceil($matches[2] * 4 / 9);
        return [
            'type' => 'FT_DECIMAL',
            'fixed_len' => $lenBytes,
            'decimal_precision' => (int)$matches[1],
            'decimal_digits' => (int)$matches[2]
        ];
    }
    
    throw new Exception("Unsupported type: $type!");
}