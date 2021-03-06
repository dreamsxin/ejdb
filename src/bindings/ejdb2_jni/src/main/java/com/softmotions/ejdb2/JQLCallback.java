package com.softmotions.ejdb2;

/**
 * SAM callback used iterate over query result set.
 */
public interface JQLCallback {

  /**
   * Called on every JSON record in result set.
   *
   * Implementor can control iteration behavior by returning a step getting next
   * record:
   *
   * <ul>
   * <li>{@code 1} go to the next record</li>
   * <li>{@code N} move forward by {@code N} records</li>
   * <li>{@code -1} iterate current record again</li>
   * <li>{@code -2} go to the previous record</li>
   * <li>{@code 0} stop iteration</li>
   * </ul>
   *
   * @param id   Current document identifier
   * @param json Document JSOn body as string
   * @return Number of records to skip
   */
  long onRecord(long id, String json);
}