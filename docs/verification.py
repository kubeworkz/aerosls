import pandas as pd

# Load your real data file
df = pd.read_csv("evaluation_data.csv")

# Compute the exact mean values to insert into your LaTeX Table (Table 1)
print(df.groupby('METRIC_TYPE')['CPU_CYCLES'].mean())

# Compute min and max values to verify table accuracy
print(df.groupby('METRIC_TYPE')['CPU_CYCLES'].agg(['min', 'max']))