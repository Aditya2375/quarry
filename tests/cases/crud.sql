CREATE TABLE employees (id INT PRIMARY KEY, name TEXT, role TEXT, salary INT);
INSERT INTO employees VALUES (1, 'Ada', 'engineer', 120);
INSERT INTO employees VALUES (2, 'Grace', 'manager', 150);
INSERT INTO employees VALUES (3, 'Linus', 'engineer', 110);
INSERT INTO employees VALUES (4, 'Edsger', 'engineer', NULL);
.tables
.schema employees
SELECT * FROM employees;
SELECT name, salary FROM employees WHERE role = 'engineer' AND salary > 100 ORDER BY salary DESC;
SELECT COUNT(*) FROM employees;
SELECT COUNT(*) FROM employees WHERE salary >= 120;
UPDATE employees SET salary = 125 WHERE id = 3;
SELECT id, salary FROM employees WHERE id = 3;
UPDATE employees SET role = 'fellow', salary = 200 WHERE name = 'Ada';
SELECT name, role, salary FROM employees WHERE id = 1;
DELETE FROM employees WHERE role = 'manager';
SELECT COUNT(*) FROM employees;
INSERT INTO employees (id, name, role) VALUES (9, 'Barbara', 'pioneer');
SELECT id, name, salary FROM employees WHERE id = 9;
