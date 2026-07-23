SELECT department_id, SUM(salary), AVG(DISTINCT bonus)
FROM employees
WHERE active = true
GROUP BY department_id, location_id
HAVING SUM(salary) > 50000;
